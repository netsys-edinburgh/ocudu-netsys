/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

/*
 * aurora_infra.h — Infrastructure metrics (container + host + GPU).
 *
 * Collected by the aurora sidecar container using eBPF (primary) or
 * procfs/sysfs/cgroup (fallback).  Data is written to a separate SHM
 * segment (Option B) so that the RAN application process is not touched.
 *
 * The aurora_reader merges infra SHM with RAN SHM at export time.
 */

#ifndef AURORA_INFRA_H
#define AURORA_INFRA_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

/* ---------------------------------------------------------------
 * Capacity constants
 * --------------------------------------------------------------- */
#define AURORA_INFRA_VERSION          1
#define AURORA_INFRA_MAX_THERMAL_ZONES  16
#define AURORA_INFRA_MAX_CPUS           128
#define AURORA_INFRA_MAX_NICS           8
#define AURORA_INFRA_MAX_GPUS           4
#define AURORA_INFRA_CONTAINER_NAME_LEN 128
#define AURORA_INFRA_PATH_LEN           256

/* ---------------------------------------------------------------
 * Backend selector (mirrors aurora_thread_backend_t pattern)
 * --------------------------------------------------------------- */
typedef enum {
    AURORA_INFRA_BACKEND_AUTO   = 0,  /* Try eBPF, fall back to procfs */
    AURORA_INFRA_BACKEND_EBPF   = 1,  /* Force eBPF (fails if unavailable) */
    AURORA_INFRA_BACKEND_PROCFS = 2,  /* Always use procfs/sysfs/cgroup */
} aurora_infra_backend_t;

/* ---------------------------------------------------------------
 * Container discovery mode
 * --------------------------------------------------------------- */
typedef enum {
    AURORA_CONTAINER_DISCOVER_MANUAL = 0,  /* Name/ID given in config */
    AURORA_CONTAINER_DISCOVER_AUTO   = 1,  /* Auto-detect from cgroup/pid ns */
} aurora_container_discover_t;

/* ---------------------------------------------------------------
 * Per-category collection interval configuration
 * All values in milliseconds.  0 = use default (infra_interval_ms).
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t cpu_interval_ms;
    uint32_t memory_interval_ms;
    uint32_t network_interval_ms;
    uint32_t disk_interval_ms;
    uint32_t host_interval_ms;     /* thermal, load, PSI, IRQ */
    uint32_t gpu_interval_ms;
} aurora_infra_intervals_t;

/* ---------------------------------------------------------------
 * Infra collection configuration
 * --------------------------------------------------------------- */
typedef struct {
    int                         enabled;         /* 0 = disabled */
    aurora_infra_backend_t      backend;
    aurora_container_discover_t discover_mode;

    /* Container identification — manual mode */
    char container_name[AURORA_INFRA_CONTAINER_NAME_LEN];

    /* SHM key for infra metrics segment (separate from RAN SHM) */
    key_t infra_shm_key;

    /* Default collection interval (ms) — used when per-category = 0 */
    uint32_t infra_interval_ms;

    /* Per-category overrides */
    aurora_infra_intervals_t intervals;

    /* GPU collection enable */
    int gpu_enabled;

    /* Host filesystem prefix for /proc, /sys mounts in sidecar container.
     * e.g. "/host" means read /host/proc, /host/sys instead of /proc, /sys.
     * Empty string = use native paths. */
    char host_proc_prefix[AURORA_INFRA_PATH_LEN];
    char host_sys_prefix[AURORA_INFRA_PATH_LEN];

    /* Cgroup v2 path for target container.
     * Auto-discovered if discover_mode = AUTO and this is empty. */
    char cgroup_path[AURORA_INFRA_PATH_LEN];

    /* Target container PID (on host PID namespace).
     * 0 = auto-discover. */
    pid_t target_pid;
} aurora_infra_config_t;

/* ===============================================================
 * METRIC STRUCTURES
 * =============================================================== */

/* ---------------------------------------------------------------
 * Container CPU metrics
 * --------------------------------------------------------------- */
typedef struct {
    double   cpu_usage_pct;         /* 0-N00% (N cores), computed from delta */
    uint64_t cpu_usage_us;          /* Cumulative user+sys microseconds (from cgroup) */
    uint64_t cpu_user_us;           /* User-space CPU time (us) */
    uint64_t cpu_system_us;         /* Kernel-space CPU time (us) */
    uint64_t cpu_throttled_us;      /* Time throttled by cgroup CPU limit (us) */
    uint32_t cpu_throttle_count;    /* Number of throttle periods */
    uint32_t cpu_nr_periods;        /* Total scheduling periods */
    uint64_t voluntary_ctxsw;       /* Voluntary context switches (delta) */
    uint64_t involuntary_ctxsw;     /* Involuntary context switches (delta) */
} aurora_container_cpu_t;

/* ---------------------------------------------------------------
 * Container memory metrics
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t mem_current_bytes;     /* Current memory usage */
    uint64_t mem_rss_bytes;         /* Resident set size (anon) */
    uint64_t mem_cache_bytes;       /* Page cache (file-backed) */
    uint64_t mem_limit_bytes;       /* Memory.max (cgroup limit, UINT64_MAX = unlimited) */
    double   mem_usage_pct;         /* current / max * 100 */
    uint64_t mem_swap_bytes;        /* Swap usage */
    uint32_t mem_oom_kill_count;    /* OOM kill events (cumulative) */
    uint64_t mem_pgfault;           /* Minor page faults (delta) */
    uint64_t mem_pgmajfault;        /* Major page faults (delta) */
    /* Pressure Stall Information (PSI) — from memory.pressure */
    float    mem_psi_some_avg10;    /* % time at least one task stalled (10s window) */
    float    mem_psi_some_avg60;
    float    mem_psi_some_avg300;
    float    mem_psi_full_avg10;    /* % time ALL tasks stalled */
    float    mem_psi_full_avg60;
    float    mem_psi_full_avg300;
} aurora_container_mem_t;

/* ---------------------------------------------------------------
 * Container network metrics (aggregated across all interfaces)
 * --------------------------------------------------------------- */
typedef struct {
    char     iface_name[32];        /* Primary interface name (eth0 etc) */
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_drops;
    uint64_t tx_drops;
    /* TCP-specific (from /proc/net/snmp) */
    uint64_t tcp_retransmits;       /* RetransSegs delta */
    uint64_t tcp_active_opens;      /* ActiveOpens delta */
    uint64_t tcp_curr_estab;        /* CurrEstab (gauge) */
    /* Cumulative link up/down transition count, summed across every non-lo
     * interface visible inside the CONTAINER's own network namespace --
     * the container-level counterpart of aurora_host_nic_t.carrier_changes.
     * Unlike /proc/<pid>/net/dev (auto-virtualized per-process by the
     * kernel), /sys/class/net/<iface>/carrier_changes is NOT exposed via
     * /proc/<pid>/, so reading it for another process's netns requires
     * briefly joining that netns with setns() -- see the dedicated helper
     * in aurora_infra_procfs.c. 0 if the join fails or the kernel lacks
     * CAP_SYS_ADMIN for it (same privilege the sidecar already needs). */
    uint64_t carrier_changes;
} aurora_container_net_t;

/* ---------------------------------------------------------------
 * Container disk I/O metrics (from cgroup io.stat)
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t io_read_bytes;
    uint64_t io_write_bytes;
    uint64_t io_read_ops;
    uint64_t io_write_ops;
    /* I/O wait — from /proc/<pid>/stat field 42 (delayacct_blkio_ticks) */
    uint64_t io_wait_us;
} aurora_container_io_t;

/* ---------------------------------------------------------------
 * Container process/FD metrics
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t open_fds;              /* Current open file descriptors */
    uint32_t fd_limit;              /* Max open files limit */
    uint32_t thread_count;          /* Number of threads */
    uint32_t process_count;         /* pids.current from cgroup */
} aurora_container_proc_t;

/* ---------------------------------------------------------------
 * COMBINED container metrics
 * --------------------------------------------------------------- */
typedef struct {
    char     container_name[AURORA_INFRA_CONTAINER_NAME_LEN];
    pid_t    container_pid;         /* PID 1 of the container (on host ns) */

    aurora_container_cpu_t   cpu;
    aurora_container_mem_t   mem;
    aurora_container_net_t   net;
    aurora_container_io_t    io;
    aurora_container_proc_t  proc;

    uint64_t timestamp_us;          /* Timestamp of last container metrics update */
} aurora_container_metrics_t;

/* ===============================================================
 * HOST-LEVEL METRICS
 * =============================================================== */

/* ---------------------------------------------------------------
 * Host thermal metrics
 * --------------------------------------------------------------- */
typedef struct {
    char     zone_type[32];         /* e.g. "x86_pkg_temp", "acpitz" */
    int32_t  temp_millicelsius;     /* Current temperature * 1000 */
    int32_t  trip_point_0_mc;       /* First trip point (throttle threshold) */
    int      valid;                 /* 1 = data present */
} aurora_thermal_zone_t;

typedef struct {
    uint32_t              num_zones;
    aurora_thermal_zone_t zones[AURORA_INFRA_MAX_THERMAL_ZONES];
} aurora_host_thermal_t;

/* ---------------------------------------------------------------
 * Host CPU metrics
 * --------------------------------------------------------------- */
typedef struct {
    float    load_1min;             /* /proc/loadavg */
    float    load_5min;
    float    load_15min;
    uint32_t runnable_tasks;        /* From /proc/loadavg field 4 (before /) */
    uint32_t total_tasks;           /* From /proc/loadavg field 4 (after /) */

    /* Per-CPU frequency (MHz) — first AURORA_INFRA_MAX_CPUS CPUs */
    uint32_t num_cpus;
    uint32_t cpu_freq_mhz[AURORA_INFRA_MAX_CPUS];

    /* CPU Pressure Stall Information */
    float    cpu_psi_some_avg10;
    float    cpu_psi_some_avg60;
    float    cpu_psi_some_avg300;

    /* Aggregate CPU utilization from /proc/stat (delta) */
    double   cpu_user_pct;
    double   cpu_system_pct;
    double   cpu_iowait_pct;
    double   cpu_idle_pct;
    double   cpu_irq_pct;
    double   cpu_softirq_pct;

    /* Softirq totals from /proc/softirqs (delta per interval) */
    uint64_t softirq_net_rx;
    uint64_t softirq_net_tx;
    uint64_t softirq_timer;
    uint64_t softirq_sched;
} aurora_host_cpu_t;

/* ---------------------------------------------------------------
 * Host memory metrics
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t mem_total_kb;
    uint64_t mem_available_kb;
    uint64_t mem_free_kb;
    uint64_t mem_buffers_kb;
    uint64_t mem_cached_kb;
    uint64_t swap_total_kb;
    uint64_t swap_free_kb;
    uint64_t hugepages_total;
    uint64_t hugepages_free;
    uint32_t hugepage_size_kb;

    /* Memory PSI */
    float    mem_psi_some_avg10;
    float    mem_psi_some_avg60;
    float    mem_psi_some_avg300;
    float    mem_psi_full_avg10;
    float    mem_psi_full_avg60;
    float    mem_psi_full_avg300;
} aurora_host_mem_t;

/* ---------------------------------------------------------------
 * Host disk metrics (from /proc/diskstats, primary disk)
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t read_ios;              /* Completed reads (delta) */
    uint64_t write_ios;             /* Completed writes (delta) */
    uint64_t read_sectors;          /* Sectors read (delta) — * 512 for bytes */
    uint64_t write_sectors;         /* Sectors written (delta) */
    uint64_t io_in_flight;          /* I/Os currently in progress (gauge) */
    uint64_t io_time_ms;            /* Time spent doing I/Os (delta ms) */
    double   disk_util_pct;         /* Utilization estimate: io_time_delta / elapsed */

    /* I/O PSI */
    float    io_psi_some_avg10;
    float    io_psi_some_avg60;
    float    io_psi_some_avg300;
    float    io_psi_full_avg10;
    float    io_psi_full_avg60;
    float    io_psi_full_avg300;
} aurora_host_disk_t;

/* ---------------------------------------------------------------
 * Host network metrics (per-NIC, first N NICs)
 * --------------------------------------------------------------- */
typedef struct {
    char     name[32];              /* Interface name */
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_drops;
    uint64_t tx_drops;
    /* Cumulative link up/down transition count, from
     * /sys/class/net/<iface>/carrier_changes. Rises on every renegotiation
     * or flap (e.g. a flapping switch port); 0 if the sysfs file is absent
     * (older kernels) or unreadable. */
    uint64_t carrier_changes;
    int      valid;
} aurora_host_nic_t;

typedef struct {
    uint32_t         num_nics;
    aurora_host_nic_t nics[AURORA_INFRA_MAX_NICS];
    /* Conntrack */
    uint32_t conntrack_count;
    uint32_t conntrack_max;
} aurora_host_net_t;

/* ---------------------------------------------------------------
 * Host kernel / misc metrics
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t entropy_avail;         /* /proc/sys/kernel/random/entropy_avail */
    double   uptime_sec;            /* /proc/uptime */
} aurora_host_kernel_t;

/* ---------------------------------------------------------------
 * COMBINED host metrics
 * --------------------------------------------------------------- */
typedef struct {
    aurora_host_thermal_t thermal;
    aurora_host_cpu_t     cpu;
    aurora_host_mem_t     mem;
    aurora_host_disk_t    disk;
    aurora_host_net_t     net;
    aurora_host_kernel_t  kernel;

    uint64_t timestamp_us;          /* Timestamp of last host metrics update */
} aurora_host_metrics_t;

/* ===============================================================
 * GPU METRICS (NVIDIA via nvidia-smi or sysfs)
 * =============================================================== */

typedef struct {
    uint32_t gpu_index;
    char     name[64];              /* e.g. "NVIDIA A100" */
    int      valid;

    /* Utilization (%) */
    uint32_t gpu_util_pct;          /* GPU core utilization */
    uint32_t mem_util_pct;          /* GPU memory bandwidth utilization */

    /* Memory (MiB) */
    uint64_t mem_used_mib;
    uint64_t mem_total_mib;

    /* Temperature and power */
    int32_t  temperature_c;         /* GPU temperature (Celsius) */
    uint32_t power_draw_mw;         /* Current power draw (milliwatts) */
    uint32_t power_limit_mw;        /* Power limit (milliwatts) */

    /* Clock speeds (MHz) */
    uint32_t clock_graphics_mhz;
    uint32_t clock_mem_mhz;

    /* PCIe throughput (KB/s) */
    uint32_t pcie_tx_kbps;
    uint32_t pcie_rx_kbps;

    /* ECC errors (cumulative) */
    uint64_t ecc_single_bit;
    uint64_t ecc_double_bit;
} aurora_gpu_metrics_t;

typedef struct {
    uint32_t             num_gpus;
    aurora_gpu_metrics_t gpus[AURORA_INFRA_MAX_GPUS];
    uint64_t             timestamp_us;
} aurora_gpu_stats_t;

/* ===============================================================
 * TOP-LEVEL INFRA METRICS (written to separate SHM segment)
 * =============================================================== */

typedef struct {
    uint32_t version;               /* AURORA_INFRA_VERSION */
    uint64_t timestamp_us;          /* Last overall update */

    aurora_container_metrics_t container;
    aurora_host_metrics_t      host;
    aurora_gpu_stats_t         gpu;

    /* Per-category timestamps to support different poll intervals */
    uint64_t last_cpu_poll_us;
    uint64_t last_mem_poll_us;
    uint64_t last_net_poll_us;
    uint64_t last_disk_poll_us;
    uint64_t last_host_poll_us;
    uint64_t last_gpu_poll_us;
} aurora_infra_metrics_t;

/* ===============================================================
 * PUBLIC API
 * =============================================================== */

/**
 * aurora_infra_init — initialise infra metrics collection.
 * Creates SHM segment, starts collector thread.
 * @cfg: configuration (copied internally)
 * Returns 0 on success, -1 on error.
 */
int aurora_infra_init(const aurora_infra_config_t *cfg);

/**
 * aurora_infra_shutdown — stop collector thread, detach SHM.
 */
void aurora_infra_shutdown(void);

/**
 * aurora_infra_get_metrics — attach to existing infra SHM by key.
 * Used by aurora_reader to read infra metrics.
 * Returns pointer to shared memory, or NULL on failure.
 */
aurora_infra_metrics_t *aurora_infra_get_metrics(key_t shm_key);

/* ---------------------------------------------------------------
 * Procfs/sysfs backend API (always available)
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_container_cpu(const aurora_infra_config_t *cfg,
                                              aurora_container_cpu_t *out);
int aurora_infra_procfs_collect_container_mem(const aurora_infra_config_t *cfg,
                                              aurora_container_mem_t *out);
int aurora_infra_procfs_collect_container_net(const aurora_infra_config_t *cfg,
                                              aurora_container_net_t *out);
int aurora_infra_procfs_collect_container_io(const aurora_infra_config_t *cfg,
                                             aurora_container_io_t *out);
int aurora_infra_procfs_collect_container_proc(const aurora_infra_config_t *cfg,
                                               aurora_container_proc_t *out);

int aurora_infra_procfs_collect_host_thermal(const aurora_infra_config_t *cfg,
                                             aurora_host_thermal_t *out);
int aurora_infra_procfs_collect_host_cpu(const aurora_infra_config_t *cfg,
                                         aurora_host_cpu_t *out);
int aurora_infra_procfs_collect_host_mem(const aurora_infra_config_t *cfg,
                                         aurora_host_mem_t *out);
int aurora_infra_procfs_collect_host_disk(const aurora_infra_config_t *cfg,
                                          aurora_host_disk_t *out);
int aurora_infra_procfs_collect_host_net(const aurora_infra_config_t *cfg,
                                         aurora_host_net_t *out);
int aurora_infra_procfs_collect_host_kernel(const aurora_infra_config_t *cfg,
                                            aurora_host_kernel_t *out);

int aurora_infra_procfs_collect_gpu(const aurora_infra_config_t *cfg,
                                    aurora_gpu_stats_t *out);

/* ---------------------------------------------------------------
 * Container discovery
 * --------------------------------------------------------------- */

/**
 * aurora_infra_discover_container — find target container cgroup path + PID.
 * Populates cfg->cgroup_path and cfg->target_pid.
 * Returns 0 on success, -1 if not found.
 */
int aurora_infra_discover_container(aurora_infra_config_t *cfg);

/**
 * aurora_infra_auto_select_container — pick container name from ran_type.
 * Queries `docker ps` and selects the first OCUDU container (ocudu_<role>) whose
 * name matches the ran_type hint. Populates cfg->container_name only if
 * currently empty. ran_type_str: "cu","du","cu_cp","cu_up","gnb".
 * Returns 0 on success, -1 if no match.
 */
int aurora_infra_auto_select_container(aurora_infra_config_t *cfg,
                                       const char *ran_type_str);

/* ---------------------------------------------------------------
 * Configuration API
 * --------------------------------------------------------------- */

/**
 * aurora_infra_parse_cli — override config from CLI arguments.
 * Scans argv for --infra-* flags and updates cfg in-place.
 * Returns number of argv entries consumed.
 */
int aurora_infra_parse_cli(aurora_infra_config_t *cfg, int argc, char **argv);

/**
 * aurora_infra_default_config — return config with sane defaults.
 */
aurora_infra_config_t aurora_infra_default_config(void);

#endif /* AURORA_INFRA_H */
