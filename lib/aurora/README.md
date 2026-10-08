# AURORA in OCUDU

Port of the OAI `openair2/AURORA` RAN metrics framework and `openair2/ANOMALY` fault injector.

Every OCUDU application (`gnb`, `odu`, `ocu`, `ocucp`, `ocuup`) can publish a live snapshot of RAN, process,
thread and (through the reader sidecar) host/container metrics into a System V shared-memory segment. The segment
holds `struct aurora_metrics` (`lib/aurora/aurora_shm.h`). Its layout is byte-identical to the OAI one, so existing
consumers attach unchanged.

## Enabling

Add an `aurora` section to the application YAML (see `configs/aurora.yml`):

```yaml
aurora:
  enable: true
  collection_interval_ms: 100      # publication period
  shm_key: 0xDEADBEEF              # unique per process on the host
  ran_type: auto                   # auto = from the application (gnb/du/cu/cu_cp/cu_up)
  thread_metrics_enabled: true
  thread_metrics_backend: auto     # auto (eBPF, fallback procfs) | ebpf | procfs
  thread_metrics_whitelist: ""     # empty = OCUDU default list (see below)
  anomaly_enabled: true            # TCP control server on 127.0.0.1:(47010 + ran_type)
```

The same options are available on the command line, for example `gnb ... aurora --enable true --shm_key 0x1234`.

## Reading

`build/utils/aurora_reader/aurora_reader`:

```bash
aurora_reader [app_config.yml] [--shm-key HEX] [--ran-type gnb|du|cu|cu_cp|cu_up] [--interval MS]
              [--output FILE.csv] [--samples N] [--once]
              [--infra-enabled yes --infra-backend auto|procfs|ebpf --infra-container NAME
               --infra-host-proc /host --infra-host-sys /host]
```

- It reads `shm_key`, `collection_interval_ms` and `ran_type` from the `aurora:` section of the given YAML.
- The RAN type defaults to the one published by the process, so `--ran-type` is optional.
- The CSV columns and per-role column gating are unchanged from OAI.
- Container auto-discovery for the infra sidecar matches the OCUDU docker-compose names: `ocudu_gnb`, `ocudu_cu`,
  `ocudu_du`, `ocudu_cu_cp` and `ocudu_cu_up`.

## Semantics vs. OAI

OAI producers wrote the segment live, and a separate thread zeroed it every interval. A reader could therefore observe
a half-filled interval, and several ~1 Hz producers raced the 100 ms reset.

In OCUDU, hooks update process-private relaxed atomics instead. The `aurora_reset` thread folds them into the segment
once per interval, so the segment always holds the last *completed* interval and `timestamp_us` changes exactly once
per interval.

The following stay the same as OAI:

| Kind of value | Fields |
|---------------|--------|
| Interval deltas | Flows, HARQ, CRC, FAPI, histograms, per-UE MAC/RLC/PDCP/GTP counters, rusage |
| Gauges | SNR, CQI, BSR, PHR, buffer occupancy, HOL delay |
| Cumulative | Handover attempts/success, starvation events |

**UE key.** OCUDU has no RNTI in RLC/PDCP/F1-U/GTP-U.

- Per-UE arrays are indexed by `ue_index % 100`. On the DU/gNB side this is `du_ue_index`; on the CU-UP/CU-CP side it
  is `cu_up_ue_index` or `cu_cp_ue_index`.
- MAC and RLC rows carry the C-RNTI.
- PDCP and GTP rows carry `ue_index + 1`, because readers treat 0 as an empty slot.

## Where each group comes from

| Group | OCUDU source |
|-------|--------------|
| 1 bhtx_in/out | `gtpu_demux_impl` N3 queue push / dequeue |
| 1 bhrx_in/out | NG-U `udp_network_gateway_impl` queue push / `sendmmsg` |
| 1 mhtx_in/out, mhrx_in/out | CU-UP `f1u_bearer_impl` (`handle_sdu`, `handle_pdu` / `handle_pdu_impl`) |
| 1 f1u_rlc, rlc_f1 | DU `f1u_bearer_impl` (`handle_pdu_impl`, `handle_sdu`) |
| 1 rlc_mac, mac_rlc | MAC `dl_sch_pdu_assembler` (`on_new_tx_sdu`), `pdu_rx_handler::handle_sdu` |
| 1 mac_dl_bo | scheduler DL buffer-state indication, summed over LCIDs per UE |
| 2 HARQ | `cell_event_manager::handle_harq_ind` (ACK / NACK / DTX per HARQ process) |
| 3 CRC | `cell_event_manager::handle_ue_crc` |
| 5, 6, 7 FAPI | `lib/scheduler/logging/aurora_sched_tap.cpp`: every PDSCH/PUSCH UE grant of each slot result |
| 4, 8, 11, 12 aggregates | publisher (`aurora_publish.cpp`) from per-UE MAC state |
| 11 sched loop / PRB use | `cell_scheduler::run_slot` decision latency, real cell PRB count and DL/UL slot count (OAI assumed 106 PRBs) |
| 11 handover | CU-CP `mobility_manager_metrics_aggregator`, keyed by source gNB-DU ID |
| 11 du_count | CU-CP `du_processor_repository` add/remove |
| 11 rsrq_avg | CU-CP serving-cell SS-RSRQ from measurement reports (always 0 in OAI) |
| 12 per-UE MAC | scheduler events (CSI, BSR, PHR, PUCCH SNR, UE create/remove), HARQ manager failures, MAC RLF detector |
| 13 RLC | RLC metrics containers (always mirrored, independent of RLC metrics reporting), TX buffer state, RX window scan |
| 14 PDCP | PDCP metrics containers, reordering (out-of-order, t-Reordering losses), crypto latency |
| 15 GTP-U | PSUP tunnel TX/RX, slot allocated per tunnel, N3 demux unknown TEID |
| 16 rusage | `getrusage(RUSAGE_SELF)` deltas |
| 17 threads | `thread/` (procfs / eBPF collectors copied from OAI) |
| 18 infra | `infra/`, run by `aurora_reader --infra-enabled yes` |

**Default thread whitelist:** `main_pool*`, `phy_worker*`, `ru_timing*`, `ru_txrx_*`, `split6*`, `radio*`,
`lower_phy*`, `ru_dummy*`, `io_broker_epoll`, `ws_server`, `aurora_reset`, `aurora_thr_proc`, `aurora_thr_ebpf`.

## ANOMALY

The ANOMALY server uses the same line protocol and ports as OAI:

- Commands: `PING`, `STATUS`, `SET target=... type=... duration_ms=...`, `CLEAR`.
- Ports: 47010 (gNB), 47011 (DU), 47012 (CU), 47013 (CU-CP), 47014 (CU-UP).
- Environment variables `AURORA_ANOMALY_PORT`, `AURORA_ANOMALY_BIND` and `AURORA_ANOMALY_TOKEN` apply as before.

The injection points sit on the real data path. In OAI, the MAC/RLC/PDCP hooks ran in the ~1 Hz statistics thread.

| Hook | Injection point | Anomaly types |
|------|-----------------|---------------|
| MAC | every scheduler slot of a cell | `L1_THREAD_CONTENTION`, `MAC_CONTENTION`, `QUEUE_TUNING` |
| RLC | every RLC TX SDU | `QUEUE_TUNING` |
| PDCP | every PDCP TX SDU | `PDCP_CONTENTION`, `QUEUE_TUNING` |

`MEMORY_LEAK` runs in its own worker thread.

## Build

- The framework is always built and stays inactive until `aurora.enable: true`.
- The eBPF backends are compiled when libbpf, clang and bpftool are found, unless `-DENABLE_AURORA_EBPF=OFF` is set.
- Running the eBPF backends needs `CAP_BPF` + `CAP_PERFMON`, or root.

## Tests

| Test | What it checks |
|------|----------------|
| `tests/unittests/aurora/aurora_core_test` | Hook → publish semantics for every group |
| `tests/unittests/aurora/aurora_e2e_test` | Publisher thread and procfs thread collector through the SysV segment |
| `du_high_test --gtest_filter=*aurora*` | Scheduler, MAC, RLC and F1-U in a real DU-high UE flow |
| `cu_up_test --gtest_filter=*aurora*` | Real N3 socket, GTP-U, PDCP |
| `cu_cp_test --gtest_filter=*aurora*` | Inter-DU handover counters |
