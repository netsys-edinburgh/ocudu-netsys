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

#ifndef AURORA_THREAD_H
#define AURORA_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#ifdef __cplusplus
/* C++ consumers (lib/aurora/aurora.cpp) only read/size this struct; the C collectors own the
 * atomic accesses. uint32_t has the same size/alignment as _Atomic uint32_t on supported ABIs. */
#define AURORA_ATOMIC_U32 uint32_t
extern "C" {
#else
#include <stdatomic.h>
#define AURORA_ATOMIC_U32 _Atomic uint32_t
#endif

/* ---------------------------------------------------------------
 * Capacity constants
 * --------------------------------------------------------------- */
#define AURORA_MAX_MONITORED_THREADS  64

/*
 * Exponential histogram bins.
 * bin[i] holds the count of scheduling quanta with runtime in
 * [2^i, 2^(i+1) - 1] nanoseconds.
 *   bin[0]  = [1, 1] ns
 *   bin[10] = [1024, 2047] ns  (~1-2 µs)
 *   bin[20] = [1048576, 2097151] ns  (~1-2 ms)
 *   bin[30] = [1073741824, 2147483647] ns  (~1-2 s)
 *   bin[31] = [2147483648, 4294967295] ns  (~2-4 s)
 * overflow  = quanta > 2^32 ns (very long blocking)
 */
#define AURORA_THREAD_HIST_BINS  32

#define AURORA_THREAD_NAME_LEN   16   /* pthread_setname_np / comm limit */
#define AURORA_PROC_NAME_LEN     16   /* /proc/self/comm limit */
#define AURORA_THREAD_WHITELIST_MAX  512  /* max chars in whitelist config string */

/* ---------------------------------------------------------------
 * Histogram type
 * --------------------------------------------------------------- */
typedef struct {
    uint64_t bin[AURORA_THREAD_HIST_BINS];
    uint64_t overflow;   /* quanta with runtime > 2^32 ns */
} aurora_thread_hist_t;

/* ---------------------------------------------------------------
 * Per-thread metrics structure
 *
 * Stored in aurora_thread_stats_t.threads[] within shared memory.
 * Written only by the AURORA collector thread; read by aurora_reader.
 *
 * Statistics mapping to the requirements table:
 *   timestamp        -> timestamp_ns
 *   Process_name     -> process_name
 *   Thread_name      -> thread_name
 *   Max              -> max_runtime_ns        (raw, nanoseconds)
 *   Total_events     -> total_events
 *   Total_runtime    -> total_runtime_ns      (derived via Procedure-1)
 *   Cpu_id           -> cpu_id
 *   mean             -> mean_ns               (derived)
 *   Range            -> range_ns              (derived: max - est_min)
 *   Variance         -> variance_ns2          (derived)
 *   Standard_Dev     -> std_deviation_ns      (derived)
 *   skewness         -> skewness              (derived: 3rd moment)
 *   kurtosis         -> kurtosis              (derived: 4th moment - 3)
 *   outliers         -> outliers_low/high     (derived: outside mean ± 2σ)
 *   irq              -> irq_count             (collected)
 * --------------------------------------------------------------- */
typedef struct {
    /*--- Identity ---*/
    uint64_t  timestamp_ns;                         /* CLOCK_MONOTONIC at last update */
    char      process_name[AURORA_PROC_NAME_LEN];   /* From /proc/self/comm */
    char      thread_name[AURORA_THREAD_NAME_LEN];  /* From /proc/self/task/{tid}/comm */
    pid_t     pid;                                  /* Process ID */
    pid_t     tid;                                  /* Kernel thread ID */
    int       cpu_id;                               /* Last observed CPU */
    int       valid;                                /* 1 = slot in use */

    /*--- Raw counters (reset each collection interval) ---*/
    uint64_t  total_events;     /* Number of scheduling quanta this interval */
    uint64_t  max_runtime_ns;   /* Maximum observed quantum runtime (ns) */

    /*--- Exponential histogram of per-quantum CPU runtimes ---*/
    aurora_thread_hist_t hist_runtime;

    /*--- Derived statistics (recomputed from histogram each interval) ---*/
    double    total_runtime_ns;   /* Calculated via Procedure-1 (uniform distribution estimate) */
    double    mean_ns;            /* Arithmetic mean of runtime distribution */
    double    range_ns;           /* max_runtime_ns - estimated_min (from lowest non-empty bin) */
    double    variance_ns2;       /* Variance σ² of runtime distribution */
    double    std_deviation_ns;   /* Standard deviation σ */
    double    skewness;           /* 3rd standardised central moment */
    double    kurtosis;           /* Excess kurtosis: 4th central moment / σ⁴ - 3 */
    uint64_t  outliers_low;       /* Events with runtime < mean - 2σ */
    uint64_t  outliers_high;      /* Events with runtime > mean + 2σ */

    /*--- IRQ and scheduling statistics ---*/
    uint64_t  irq_count;             /* IRQ/softirq count while thread was running */
    uint64_t  voluntary_ctxsw;       /* Voluntary context switches (delta per interval) */
    uint64_t  involuntary_ctxsw;     /* Involuntary context switches / preemptions (delta) */
    uint64_t  cpu_migrations;        /* CPU migrations (delta per interval) */

    /*--- Internal: delta computation state (not for external consumers) ---*/
    uint64_t  _prev_utime_ticks;   /* Previous utime in clock ticks (procfs fallback) */
    uint64_t  _prev_stime_ticks;   /* Previous stime in clock ticks */
    uint64_t  _prev_vol_sw;        /* Previous voluntary ctxsw count */
    uint64_t  _prev_nonvol_sw;     /* Previous non-voluntary ctxsw count */
    int       _prev_cpu;           /* CPU on previous poll (for migration detection) */

    /*--- Padding to keep struct size a multiple of 8 ---*/
    uint8_t   _pad[4];
} aurora_thread_metrics_t;

/* ---------------------------------------------------------------
 * Collection backend selector
 * --------------------------------------------------------------- */
typedef enum {
    AURORA_THREAD_BACKEND_AUTO   = 0,  /* Try eBPF, fall back to procfs */
    AURORA_THREAD_BACKEND_EBPF   = 1,  /* Force eBPF (fails if unavailable) */
    AURORA_THREAD_BACKEND_PROCFS = 2,  /* Always use procfs polling */
} aurora_thread_backend_t;

/* ---------------------------------------------------------------
 * Thread collection configuration
 * (Extension of aurora_config_t; embedded as thread_cfg field)
 * --------------------------------------------------------------- */
typedef struct {
    int                     enabled;     /* 0 = disabled, 1 = enabled */
    aurora_thread_backend_t backend;
    /* Comma-separated thread name patterns; '*' matches any suffix.
     * e.g. "L1_rx_thread,L1_tx_thread,ru_thread,Tpool*,GTPrx_*" */
    char whitelist[AURORA_THREAD_WHITELIST_MAX];
} aurora_thread_config_t;

/* ---------------------------------------------------------------
 * Container — appended to struct aurora_metrics
 * --------------------------------------------------------------- */
typedef struct {
    AURORA_ATOMIC_U32         num_threads;  /* Number of valid (in-use) slots */
    aurora_thread_metrics_t   threads[AURORA_MAX_MONITORED_THREADS];
} aurora_thread_stats_t;

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

/**
 * aurora_thread_init — start the thread metrics collection subsystem.
 * Must be called after aurora shared memory is attached.
 * @cfg:  collection configuration (copied internally)
 * @ts:   pointer to aurora_thread_stats_t inside shared memory
 * Returns 0 on success, -1 on error.
 */
int aurora_thread_init(const aurora_thread_config_t *cfg, aurora_thread_stats_t *ts);

/**
 * aurora_thread_shutdown — stop the collector thread.
 */
void aurora_thread_shutdown(void);

/**
 * aurora_thread_reset_interval_stats — zero per-interval counters and histograms.
 * Called by aurora_reset_thread() each collection interval.
 * Derived statistics and identity fields are preserved.
 */
void aurora_thread_reset_interval_stats(aurora_thread_stats_t *ts);

/**
 * aurora_thread_compute_all_stats — recompute derived statistics for all valid slots.
 * Called after a collection round, before the reset.
 */
void aurora_thread_compute_all_stats(aurora_thread_stats_t *ts);

/**
 * aurora_thread_compute_stats — recompute derived statistics for one thread slot.
 */
void aurora_thread_compute_stats(aurora_thread_metrics_t *m);

/**
 * aurora_thread_find_slot — find existing slot for @tid.
 * Returns slot index or -1 if not found.
 */
int aurora_thread_find_slot(aurora_thread_stats_t *ts, pid_t tid);

/**
 * aurora_thread_name_matches — return 1 if @name matches the configured whitelist.
 */
int aurora_thread_name_matches(const char *name);

/**
 * aurora_thread_record_quantum — record a single scheduling quantum runtime (ns).
 * Updates histogram bin, total_events, max_runtime_ns.
 * Called from the eBPF user-space agent or procfs delta path.
 */
void aurora_thread_record_quantum(aurora_thread_metrics_t *m, uint64_t runtime_ns);

#ifdef __cplusplus
}
#endif

#endif /* AURORA_THREAD_H */
