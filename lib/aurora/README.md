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

`AURORA_WARMUP_S` (or `--warmup <s>`) is read by `aurora_reader` itself: the samples of the first `<s>` seconds after
the RAN process' first publication are dropped, from CSV and InfluxDB alike (e.g. until the cell is up and the UEs are
attached again after a restart; `anomaly-ctl` relies on it).

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
| `aurora_anomaly` | `anomaly_label` (string field, set by the anomaly-injector sidecar) | | column name |
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
| `ext.anomaly_label` | Version 2. Anomaly label set by the anomaly-injector sidecar, see [Anomaly label](#anomaly-label). |

The CSI quantities in `ext.csi[100]` are:
- CRI/SSBRI and L1-RSRP lists;
- RI, LI;
- wideband CQI for TB1 and TB2;
- subband differential CQIs;
- PMI for the two-port, Type-I single-panel (i1,1, i1,2, i1,3, i2) and Type-II (i1,1, i1,2, and per layer i1,3, i1,4,
  i2,1, i2,2) codebooks.

Quantities missing from the last report read `0xFF` (`AURORA_CSI_NOT_REPORTED`).

RC cell 0 `handover_*` is the total over all DUs. `aurora_reader` appends the extension CSV columns (`csi_*`,
`ul_failure_*`, `ho_*`) after all OAI columns, then `anomaly_label` as the last column, and prints the tables with
`--once`.

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

- Commands: `PING`, `STATUS`, `SET target=... type=... duration_ms=...`, `CLEAR`, and (OCUDU) `LABEL value=...`.
- OCUDU: `PING` and `STATUS` replies carry `started_us`, the wall-clock time the anomaly server (and with it the AURORA
  publisher) started, i.e. where the samples and aurora_reader's warm-up begin.
- Ports: 47010 (gNB), 47011 (DU), 47012 (CU), 47013 (CU-CP), 47014 (CU-UP).
- Environment variables `AURORA_ANOMALY_PORT`, `AURORA_ANOMALY_BIND` and `AURORA_ANOMALY_TOKEN` apply as before.

The server binds to `127.0.0.1` by default, so an injector sidecar in the same pod (shared network namespace) reaches
it without exposing the port. Example, a MAC contention rising from 10 % to 80 % over one minute:

```bash
echo "SET type=MAC_CONTENTION duration_ms=60000 load_min=10 load_max=80 step_ms=5000" | nc 127.0.0.1 47010
```

### MAC_CONTENTION and PDCP_CONTENTION

These start `stress-ng` on the CPUs of the dedicated `mac_sched#<cell>` or `pdcp` worker, with the worker's own
scheduling policy and priority. With `SCHED_FIFO` at the same priority neither preempts the other: each keeps the CPU
until it blocks, so the worker waits for up to one stressor busy slice whenever it wakes up during one.

- The workers are found by thread name and must exist: enable `expert_execution.threads.mac_sched` (gNB, DU) or
  `expert_execution.threads.pdcp` (gNB, CU, CU-UP), see `configs/dedicated_layer_workers.yml`. Otherwise `SET` returns
  `ERR code=NO_WORKER`.
- Give each worker its own CPUs (`expert_execution.affinities.mac_sched_cpus` / `pdcp_cpus`). Without them it falls
  back to the main thread pool CPUs, and the stressor hits the pool as well; the `SET` reply then carries
  `warning=other_rt_threads_share_these_cpus`.
- `stress-ng` must be on `PATH` (it is in the Docker images), or set `AURORA_ANOMALY_STRESS_NG` to its path. It is
  started by the RAN process itself, so it lands in the same container and CPU set as the worker.

| Parameter | Default | Meaning |
|-----------|---------|---------|
| `duration_ms` | 10000 | stressor lifetime (max 1 h); `CLEAR` stops it earlier |
| `load` | 50 | constant busy share of the stressor in percent (1-100), when no ramp is given |
| `load_min` | `load` | ramp start in percent (1-100) |
| `load_max` | `load_min` | ramp end in percent (1-100); the load rises linearly from `load_min` to `load_max` over `duration_ms` |
| `step_ms` | 1000 | ramp step (>= 100). stress-ng cannot change its load while running, so each step restarts it, leaving a gap of a few ms |
| `slice_ms` | 1 | busy slice in ms (1-100), i.e. the longest wait it imposes on the worker per wake-up |
| `prio_offset` | 0 | stressor priority relative to the worker (-10..10). `1` preempts the worker: a deterministic CPU steal of `load` % instead of competition |
| `cell` | all | MAC only: target `mac_sched#<cell>` |

The reply reports what was targeted, e.g. `OK applied=1 ... threads=1 cpus=12 policy=SCHED_FIFO worker_prio=97
stress_prio=97 load_min=10 load_max=80 step_ms=5000 steps=12 slice_ms=1 pid=1234 shared_rt_threads=0`. It also reports `load_min`, `load_max`,
`step_ms` and `steps`. `STATUS` adds `mac_stress_cpus`, `mac_stress_load`, `pdcp_stress_cpus` and `pdcp_stress_load`
(the load of the running step). stress-ng also gets `--timeout duration + 2 s`, so it stops even if the RAN process dies.

`SCHED_DEADLINE` is not offered: the kernel does not allow a deadline task to be pinned to a subset of the CPUs.

### Anomaly label

The `anomaly_label` of every sample comes from the anomaly-injector sidecar only, so it can also name anomalies the
RAN process cannot inject itself (network impairment, host stress, ...):

- `LABEL value=<label>` sets it: 1-31 characters of `a-z 0-9 _ + -`, e.g. `mac_contention`, `link_flap`, `normal`.
  The reply is `OK label=<label> changed=<0|1>`, and `STATUS` shows it as `label=`.
- The RAN process keeps the label until the next `LABEL`, and the publisher stamps it on every sample
  (`ext.anomaly_label`). It is `normal` from the start until the first `LABEL`. `SET` / `CLEAR` never change it.
- `aurora_reader` writes it as the last CSV column, `anomaly_label`, and as the `anomaly_label` field of the
  `aurora_anomaly` InfluxDB measurement. It reads `unknown` from a producer older than extension version 2.
- The sidecar sends the label when it changes and re-sends it every second, so a restarted RAN process (label back to
  `normal`) gets it again within a second. One command costs the RAN process about 15 us of CPU on the listener
  thread, which runs at normal priority and never preempts the real-time workers.

A sample is labelled by the label the RAN process holds when the sample is published. For an anomaly injected by
another tool, set the label right before injecting it and back to `normal` right after stopping it.

### anomaly-injector sidecar (`docker/anomaly_injector/`)

`utils/anomaly_injector/anomaly_injector.py` runs as a sidecar in the RAN pod (shared network namespace), image
`24whitedevil/netsys-ocudu-anomaly-injector`. As a daemon it holds the label, plays an optional schedule, and offers a
control port, `127.0.0.1:47020` (`ANOMALY_INJECTOR_PORT`), with the same line protocol: `LABEL value=...` changes the
daemon's label, every other line is forwarded to the RAN process. Tools in other containers of the pod label their
anomalies there, because a label sent directly to the RAN process would be overwritten by the daemon's next re-send.

```bash
anomaly-injector                                                            # daemon: run if $ANOMALY_SCHEDULE is set, else idle
anomaly-injector label link_flap                                            # through the daemon (direct if none runs)
echo "LABEL value=normal" | nc 127.0.0.1 47020                              # same, from any container of the pod
anomaly-injector send SET type=PDCP_CONTENTION duration_ms=30000 load=40   # one command to the RAN process
anomaly-injector status
```

A schedule is JSON, given as a file path or inline in `ANOMALY_SCHEDULE`. Either fixed steps, see
`docker/anomaly_injector/schedule.example.json`:

```json
{"repeat": true, "start_delay_ms": 30000, "steps": [
  {"type": "NONE", "duration_ms": 60000},
  {"type": "MAC_CONTENTION", "duration_ms": 120000, "load_min": 10, "load_max": 80, "step_ms": 10000},
  {"type": "LABEL_ONLY", "label": "link_flap", "duration_ms": 30000}]}
```

- `NONE`: a normal period, label `normal`.
- `LABEL_ONLY`: sets `label` for the step; the anomaly itself comes from elsewhere.
- Any other type is sent as `SET` with the other keys as parameters. Its label is `label`, by default the lower-case
  type (`mac_contention`). It is set once the RAN process has confirmed the `SET`, and back to `normal` after the
  step's `CLEAR`, so labelled samples always lie inside the anomaly.

Or random RAN-injected anomalies separated by normal gaps, each range drawn uniformly:

```json
{"random": {"types": ["MAC_CONTENTION", "PDCP_CONTENTION"], "duration_ms": [30000, 120000],
            "gap_ms": [30000, 90000], "load_min": [5, 30], "load_max": [40, 95], "step_ms": 5000,
            "count": 0, "seed": 1}}
```

The daemon waits until the RAN process answers `PING` and starts from label `normal`. It logs every command and reply
to stdout as JSON lines with the wall-clock time (`ts_us`, comparable with `timestamp_us`); the periodic re-sends are
not logged. After a schedule it keeps holding the label. On SIGTERM it changes nothing: the pod, RAN process included,
is stopping, and the samples published meanwhile keep the current label. Environment: `AURORA_ANOMALY_HOST` (127.0.0.1), `AURORA_ANOMALY_PORT` (47010;
47011 DU, 47012 CU, 47013 CU-CP, 47014 CU-UP), `AURORA_ANOMALY_TOKEN`, `ANOMALY_SCHEDULE`, `ANOMALY_CONNECT_TIMEOUT_S`
(0 = wait forever), `ANOMALY_LABEL_RESEND_S` (1; 0 = only on change), `ANOMALY_INJECTOR_PORT` (47020; 0 = none),
`ANOMALY_RESTART_DEPLOYMENT` (watchdog, below).

Control-port commands besides `LABEL` and forwarding (also as `anomaly-injector inject|lease|daemon-status`):

| Command | Effect |
|---------|--------|
| `INJECT label=<label> type=<TYPE> <SET parameters>` | `SET` to the RAN process; on `OK` sets the label (default: lower-case type) |
| `LABEL value=<label> duration_ms=<ms>` | as `LABEL`, and tells the watchdog when the anomaly (injected elsewhere) ends |
| `LEASE ttl_s=<s>` | renews the controller lease |
| `DAEMON` | `OK label=... lease_s=<left> anomaly_s=<left> fronthaul_mbps=<rate> bursts=<lane:on/off/count,...>` |

**Bursts.** An `INJECT` (or `MULTI`, or one anomaly of a `MULTI`) with `burst_ms` is not one continuous anomaly: inside
its `duration_ms` the sidecar plays repeated short bursts, so the controller only sets the segment and the sidecar
decides when each burst runs.

```text
INJECT label=pdcp_preemption type=PDCP_CONTENTION duration_ms=3500000 burst_ms=800-6000 period_ms=10000 load_min=92 load_max=98 prio_offset=1
```

- `burst_ms` and `period_ms` (start to start) are ranges (`min-max`, or one value) drawn for every burst. The level is
  drawn per burst from `load_min`..`load_max` (or `load=lo-hi`), `rate_min_mbps`..`rate_max_mbps` for `FRONTHAUL`. Other
  keys (`prio_offset`, `slice_ms`, ...) go to `SET` unchanged.
- Each burst is one `SET` of the burst's length plus 300 ms, then `CLEAR`. The label is set once the burst is applied
  and back to `normal` before it is stopped, so labelled samples lie inside the anomaly.
- In a burst `MULTI` every anomaly bursts on its own schedule (`burst_ms` / `period_ms` at the top level are the
  default) and the label names the anomalies active at that moment, joined by `+`: `mac`, `pdcp`, `fh` by default, e.g.
  `mac+fh` (a sub-anomaly's `label` overrides; every combination must fit 31 characters).
- Every burst is logged as a `burst` event with `label`, `level`, `dur_ms`, `start_us` and `end_us` (same clock as the
  AURORA `timestamp_us`).
- The first burst of every anomaly is applied before the reply (all-or-nothing), so a bad parameter fails the `INJECT`;
  the reply carries the segment's `duration_ms` (anomaly-ctl's clamp guard). A new `INJECT` / `MULTI` or
  `CLEAR type=ALL` ends the bursts; with a lost lease they run to the end of the segment, which leaves the label
  `normal`.

In an anomaly-ctl campaign the same keys go in the segment, ranges as strings or lists:
`{"type": "PDCP_CONTENTION", "duration_s": 3500, "burst_ms": [800, 6000], "period_ms": 10000, "load_min": 92,
"load_max": 98, "prio_offset": 1, "label": "pdcp_preemption"}`.

**Watchdog.** Armed by the first `LEASE`. If the lease runs out while the label is not `normal`, the sidecar lets the
current anomaly end (its `duration_ms`), sends `CLEAR type=ALL`, labels `recovery` and restarts its own pod: it patches
the pod template of `ANOMALY_RESTART_DEPLOYMENT` like `kubectl rollout restart`, through the Kubernetes API with the
pod's service account (token, CA and namespace under `/var/run/secrets/kubernetes.io/serviceaccount`). That needs a
Role with `patch` on that Deployment. The restarted pod has no lease, so it stays `normal`. With the label `normal`
the watchdog does nothing.

### anomaly-ctl (management node)

`utils/anomaly_injector/anomaly_ctl.py` (also `/usr/local/bin/anomaly-ctl` in the injector image) plays campaigns on a
RAN pod through `kubectl` only:

```bash
anomaly-ctl --log run.jsonl run campaign.json      # namespace / deployment in the file, or -n / -d
anomaly-ctl -n <ns> -d <deployment> status | restart | label <label> | send <line>
```

For every segment (`NONE`, `LABEL_ONLY`, or an anomaly type with its `SET` parameters, `duration_s` recorded):

1. `kubectl rollout restart`, wait for Ready and for the RAN process to answer; retried `--restart-retries` (5) times
   with `--ready-timeout` (300 s) each, then the campaign is aborted.
2. Wait for the warm-up: `AURORA_WARMUP_S`, read from the Deployment.
3. `--inject-lead` (3 s) before it ends, `inject` / `label` through the sidecar, with `--margin` (30 s) more anomaly
   than the segment needs, so the anomaly still runs when the next restart stops the pod.
4. Record `duration_s`; the log gets a `segment_recorded` line with the label and `record_start_us` /
   `record_end_us`.

It renews the sidecar lease every 10 s (valid 60 s) and, at the end or on Ctrl-C, restarts the pod once more and leaves
it `normal`. Campaign files may also use `"random"` (anomaly types, `duration_s` / `gap_s` / load ranges, `count`,
`seed`) and `"repeat"` (0 = forever). See the docstring for the format; the campus5g-ng deployment and campaigns are
in `metrics/anomaly/` of campus5g-ng.

### Other anomaly types

They are injected on the data path. In OAI, the MAC/RLC/PDCP hooks ran in the ~1 Hz statistics thread.

| Hook | Injection point | Anomaly types |
|------|-----------------|---------------|
| MAC | every scheduler slot of a cell | `L1_THREAD_CONTENTION` (`delay_us`), `QUEUE_TUNING` (`jitter_us`) |
| RLC | every RLC TX SDU | `QUEUE_TUNING` |
| PDCP | every PDCP TX SDU | `QUEUE_TUNING` |

`MEMORY_LEAK` (`bytes_per_sec`) runs in its own worker thread.

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
