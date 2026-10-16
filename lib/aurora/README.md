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
  # cpus: 15                       # aurora_reset publisher CPUs (default: expert_execution main_pool_cpus)
```

The same options are available on the command line, for example `gnb ... aurora --enable true --shm_key 0x1234`.

**Use a different `shm_key` for every process on a host** (e.g. DU, CU-CP and CU-UP).
- AURORA refuses to start if another live process is attached to a segment with the same key, and logs an error.
- A stale segment left behind by a crashed process is replaced.
- The segment is created with mode `0644`; readers attach it read-only.

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

## Sidecar container (`docker/aurora_reader/`)

Build from the repository root:

```bash
docker build -f docker/aurora_reader/Dockerfile -t ocudu/aurora_reader .
```

- **Base image.** Multi-stage on Debian bookworm, which ships libbpf 1.1, bpftool and clang, so the eBPF infra backend
  is built in. The runtime image is about 176 MB.
- **What gets built.** Only `lib/aurora` and `aurora_reader`, through the standalone superproject
  `docker/aurora_reader/CMakeLists.txt`.
- **Build context.** `Dockerfile.dockerignore` limits it to the four source directories (about 4 MB), even with a build
  tree in the repo.

With docker-compose, run from `docker/`:

```bash
docker compose -f docker-compose.yml -f aurora_reader/docker-compose.aurora.yml up
```

- The override joins the gNB IPC namespace (`ipc: service:gnb`, with `gnb` set to `ipc: shareable`). It also runs the
  sidecar with `pid: host` and privileged mode, and mounts the host `/proc`, `/sys`, the docker socket, BTF and tracefs
  for the infra backends.
- CSV files go to `docker/aurora-data/`.
- The gNB config must contain `aurora: { enable: true, shm_key: ... }` with the same key as `AURORA_SHM_KEY`.
- For split deployments, add one sidecar per RAN container, each with its own `ipc: service:<name>`,
  `AURORA_SHM_KEY` and `AURORA_INFRA_CONTAINER`.

The entrypoint (`aurora-sidecar`) builds the `aurora_reader` command line from environment variables:
- `AURORA_CONFIG`, `AURORA_SHM_KEY`, `AURORA_RAN_TYPE`, `AURORA_INTERVAL_MS`, `AURORA_SAMPLES`, `AURORA_OUTPUT`;
- `AURORA_INFRA`, `AURORA_INFRA_BACKEND`, `AURORA_INFRA_CONTAINER`, `AURORA_INFRA_INTERVAL`, `AURORA_HOST_PREFIX`;
- `AURORA_WAIT_S`.

It waits until the RAN process has created its segment, so the start order does not matter. Arguments given to the
container are passed to `aurora_reader` unchanged, e.g. `docker run ... ocudu/aurora_reader --once`.

### InfluxDB output

`aurora_reader` can send every sample to InfluxDB instead of, or as well as, the CSV file, through a Telegraf in the
same pod (`inputs.socket_listener` with `data_format = "influx"`, `outputs.influxdb_v2`). Telegraf batches the writes,
retries while InfluxDB is unreachable and holds the token.

| Option | Environment | Default |
|--------|-------------|---------|
| `--output-mode csv\|influx\|both` | `AURORA_OUTPUT_MODE` | `csv` |
| `--influx-url udp://host:port\|tcp://host:port\|unix:///path` | `AURORA_INFLUX_URL` | `udp://127.0.0.1:8094` |
| `--influx-tag k=v` (repeatable) | `AURORA_INFLUX_TAGS=k=v,k=v` | none |

- One point per sample and measurement, timestamped with `timestamp_us` (nanosecond precision in the line protocol).
  The fields are the CSV columns, with the same values; empty and non-finite values are left out.
- Every point carries `source=aurora`, `ran_component=<gnb|du|cu|cu-cp|cu-up>` and the extra tags.
- Measurements, by CSV column prefix:

| Measurement | Columns | Extra tag | Field key |
|-------------|---------|-----------|-----------|
| `aurora_flow` | Group 1 flows (`bhtx_in_*` ... `mac_dl_bo*`) | `flow` | column suffix (`bytes`, `pkts`, ...) |
| `aurora_thread` | `THREAD_<name>_*` | `thread` (name; `<name>_<tid>` for a repeated name) | metric (`total_events`, ...) |
| `aurora_kpm` | `kpm_*` | | KPM name (`RRU.PrbTotDl`, ...) |
| `aurora_f1c`, `aurora_f1u`, `aurora_rlc`, `aurora_pdcp`, `aurora_gtp`, `aurora_handover`, `aurora_rc`, `aurora_csi`, `aurora_rusage`, `aurora_infra` | `f1c_`, `f1u_`, `rlc_`, `pdcp_`, `gtp_`, `ho_`, `rc_`/`rc0_`, `csi_`, `ru_`, `infra_` | | column name |
| `aurora_mac` | every other column (HARQ, CRC, SINR/CQI/BSR, FAPI, MAC aggregates, ...) | | column name |

- UDP sends are split into datagrams of at most 32 kB at line boundaries. A stream (TCP, Unix) connection that drops
  is reopened at most once per second; rows that cannot be sent are dropped and counted, and the counts are printed
  at exit.
- Set `omit_hostname = true` in Telegraf, otherwise it adds its own `host` tag.

## OCUDU extension (`struct aurora_metrics_ext`)

The extension is appended as the last member of `struct aurora_metrics`, so the OAI prefix stays byte-identical.
Check `ext.version` (`AURORA_EXT_VERSION`) and `ext.size` before using it.

| Field | Content |
|-------|---------|
| `ext.csi[100]` | Full last CSI report per UE, with interval report counters (total / invalid). It carries every quantity OCUDU's CSI decoder supports (`csi_report_data`). |
| `ext.ho_du[16]` | Handover executions per source gNB-DU ID: interval and cumulative attempts/successes, plus the success rate. |
| `ext.ho_prep_*` | N2 handover preparations (CU-CP level). |

The CSI quantities in `ext.csi[100]` are:
- CRI/SSBRI and L1-RSRP lists;
- RI, LI;
- wideband CQI for TB1 and TB2;
- subband differential CQIs;
- PMI for the two-port, Type-I single-panel (i1,1, i1,2, i1,3, i2) and Type-II (i1,1, i1,2, and per layer i1,3, i1,4,
  i2,1, i2,2) codebooks.

Quantities missing from the last report read `0xFF` (`AURORA_CSI_NOT_REPORTED`).

RC cell 0 `handover_*` is the total over all DUs. `aurora_reader` appends the extension CSV columns (`csi_*`,
`ul_failure_*`, `ho_*`) after all OAI columns, and prints both tables with `--once`.

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
| 11 handover | CU-CP `mobility_manager_metrics_aggregator`: per source gNB-DU ID in `ext.ho_du[]` (totals in RC cell 0); N2 preparations in `ext.ho_prep_*` |
| 11 du_count | CU-CP `du_processor_repository` add/remove |
| 11 rsrq_avg | CU-CP serving-cell SS-RSRQ from measurement reports (always 0 in OAI) |
| 12 per-UE MAC | scheduler events (CSI, BSR, PHR, PUCCH SNR, UE create/remove), HARQ manager failures, MAC RLF detector |
| 13 RLC | RLC metrics containers (always mirrored, independent of RLC metrics reporting), TX buffer state, RX window scan |
| 14 PDCP | PDCP metrics containers, reordering (out-of-order, t-Reordering losses), crypto latency |
| 15 GTP-U | PSUP tunnel TX/RX, slot allocated per tunnel, N3 demux unknown TEID |
| 16 rusage | `getrusage(RUSAGE_SELF)` deltas |
| 17 threads | `thread/` (procfs / eBPF collectors copied from OAI) |
| 18 infra | `infra/`, run by `aurora_reader --infra-enabled yes` |
| F1-C (`f1c_*`) | F1-C gateways (`lib/f1ap/gateways/f1c_aurora_tap.cpp`): CU-CP SCTP server, DU SCTP client, co-located connector |
| F1-U (`f1u_*`) | CU-UP F1-U TX/RX metric helpers; DU `f1u_bearer_impl` in a DU-only process |

**F1-C and F1-U.** These fields are part of `struct aurora_metrics` (after `thread_stats`, before `ext`). DL is
CU-CP/CU-UP to DU, UL is DU to CU-CP/CU-UP.

- F1-C: F1AP messages, encoded bytes and largest message per direction; RRC Message Transfer messages and bytes (SRB
  traffic on F1-C); decode errors, Initial UL RRC Message Transfers, Paging, DU-initiated UE Context Release Requests,
  F1 Resets, Error Indications; F1 links up; and for F1 Setup, UE Context Setup, Modification and Release the
  attempts, successes, failures and request-to-outcome latency (matched by gNB-CU UE F1AP ID or transaction ID). A
  request without outcome after 5 s counts in `f1c_proc_timeouts`. Each message is counted once per process, at the
  F1-C gateway; the co-located gNB connector encodes the messages to measure them while AURORA is active.
- F1-U: NR-U DL/UL PDUs, drops per direction, PDCP discards requested towards the DU, Data Delivery Status messages
  and their handling failures. Counted on the CU-UP side, or on the DU side in a DU-only process, so a gNB counts each
  PDU once. The byte counts stay in Group 1 (`mhtx_*`, `mhrx_*`, `f1u_rlc`, `rlc_f1`).

**Default thread whitelist.** It covers every thread name the OCUDU code base sets, as they appear in
`/proc/<pid>/task/<tid>/comm` (`unique_thread` truncates to 15 characters). A test checks it against the real names
(`aurora_core_test`, `aurora_thread_whitelist.*`).

| Source | Names |
|--------|-------|
| Main thread of each app | `gnb`, `odu`, `ocu`, `ocucp`, `ocuup`, `odu_low`. The ocudulog backend thread and unnamed library threads inherit this name. |
| `apps/services/worker_manager` | `main_pool#*`, `phy_worker`, `radio`, `split6`, `ru_timing`, `ru_txrx_#*`, `ru_dummy`, `lower_phy#*`, `lower_phy_tx#*`, `lower_phy_rx#*`, `lower_phy_ul#*` |
| Optional dedicated layer workers (`expert_execution.threads`) | `mac_sched#*` (MAC scheduler, one per cell), `pdcp` (CU-UP PDCP), `cu_cp` (CU-CP, SRB PDCP included) |
| Support threads | `io_broker_epoll`, `ws_server` (remote control), `tracer_worker` (event tracing), `rx_symb_print` (PHY RX grid printer) |
| `ocudu_phy_tap` and libzmq | `SRSIQDump`, `ULPhyTap`, `ZMQbg/*` |
| AURORA | `aurora_reset` |
| `apps/examples` | `async_thread`, `lower_phy`, `lower_phy_dl`, `lower_phy_ul`, `lower_phy_prach`, `low_rx`, `low_tx`, `low_dl`, `low_ul`, `ru_rx_#*`, `ru_emu_#*`, `ru_emulator`, `radio_ssb`, `radio_util_samp`, `rx_power_analyz` |

Whitespace and newlines around entries are ignored, so the list can span several YAML lines. The registry keeps at most
64 patterns and tracks at most 64 threads.

`aurora_reader` appends `_<tid>` to the CSV columns of threads whose name is shared with another thread.

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
- The eBPF backends are compiled when libbpf >= 1.0, clang and bpftool are found, unless
  `-DENABLE_AURORA_EBPF=OFF` is set.
- **libbpf >= 1.0 is required.** Kernels >= 6.0 publish `BTF_KIND_ENUM64` in `/sys/kernel/btf/vmlinux`, which older
  libbpf cannot parse. Loading then fails at runtime with `failed to find valid kernel BTF`.
- Ubuntu 22.04 ships libbpf 0.5. Build a newer one locally and point CMake at it:

  ```bash
  git clone --depth 1 --branch v1.4.0 https://github.com/libbpf/libbpf.git build/third_party/libbpf-src
  mkdir -p build/third_party/libbpf-obj
  make -C build/third_party/libbpf-src/src BUILD_STATIC_ONLY=1 OBJDIR=$PWD/build/third_party/libbpf-obj \
       DESTDIR=$PWD/build/third_party/libbpf PREFIX=/ LIBDIR=/lib install
  cmake -B build -DAURORA_LIBBPF_DIR=$PWD/build/third_party/libbpf
  ```

- Running the eBPF backends needs root, or `CAP_BPF` + `CAP_PERFMON` (plus `CAP_SYS_ADMIN` for the infra cgroup
  lookup).
- Docker images built from `docker/Dockerfile` install the eBPF build and runtime dependencies on Debian and Ubuntu
  (`docker/scripts/install_aurora_ebpf_dependencies.sh`) and give the RAN binaries the `cap_bpf` and `cap_perfmon`
  file capabilities. The container must allow both, e.g. `privileged: true` or `cap_add: [BPF, PERFMON]`, otherwise
  the binary fails to start with "operation not permitted".
- The thread collectors (procfs or eBPF) are driven by the publisher once per interval, so every published interval is
  complete. OAI used a free-running collector thread that raced the reset and could destroy the BPF skeleton while still
  reading its maps.
- **eBPF counters are cumulative.** The BPF programs never reset the per-TID entries, and user space never writes to
  them: each interval it looks up the TID of every monitored slot (no map walk) and publishes the deltas against the
  previous totals. The per-interval maximum comes from an `interval_epoch` map that user space advances once per
  interval. Entries of exited threads are deleted when their slot is freed.
  - OAI reset every entry while walking the hash map with `get_next_key`. An update moves the element to the head of
    its bucket, so two TIDs in one bucket made the walk loop forever and hung the publisher.
- **eBPF voluntary/involuntary context switches** follow the kernel's `nvcsw`/`nivcsw` rule: a switch is involuntary
  when the task was still runnable (`prev_state` "R" or "R+").
  - OAI counted only "R+" (`0x100`) as involuntary.
  - User-space threads are preempted on return to user mode and report plain "R", so OAI counted every preemption as
    voluntary.

## Tests

| Test | What it checks |
|------|----------------|
| `tests/unittests/aurora/aurora_core_test` | Hook → publish semantics for every group |
| `tests/unittests/aurora/aurora_e2e_test` | Publisher thread and procfs thread collector through the SysV segment |
| `du_high_test --gtest_filter=*aurora*` | Scheduler, MAC, RLC and F1-U in a real DU-high UE flow |
| `cu_up_test --gtest_filter=*aurora*` | Real N3 socket, GTP-U, PDCP |
| `cu_cp_test --gtest_filter=*aurora*` | Inter-DU handover counters |
