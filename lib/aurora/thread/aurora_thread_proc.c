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

#define _GNU_SOURCE  /* pthread_setname_np */

/*
 * aurora_thread_proc.c — procfs-based thread metrics collector.
 *
 * Polling loop (runs in a dedicated pthread):
 *   Each collection_interval_ms:
 *   1. aurora_thread_discover()  — scan /proc/self/task/, add new threads
 *   2. For each registered thread:
 *      a. Read /proc/self/task/{tid}/stat  — CPU time (jiffies), last CPU
 *      b. Read /proc/self/task/{tid}/status — voluntary/nonvoluntary ctxsw
 *      c. Compute delta CPU time (ns) and record into histogram
 *      d. Update context switch / CPU migration counters
 *   3. aurora_thread_compute_all_stats() — recompute derived statistics
 *
 * Limitations of the procfs path (vs eBPF):
 *   - CPU time resolution is 1/HZ (typically 10 ms with CONFIG_HZ=100,
 *     or 1 ms with CONFIG_HZ=1000).  Not suitable for nanosecond-level
 *     scheduling latency of real-time PHY threads.
 *   - Does not count per-quantum IRQ interruptions.
 *   - IRQ count is approximated as involuntary_ctxsw (preemptions).
 *
 * This module is always compiled.  When eBPF is available, the eBPF agent
 * (aurora_thread_ebpf.c) takes over collection and this thread is not started.
 */

#include "aurora_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>

/* External declarations from aurora_thread_registry.c */
void aurora_thread_registry_init(const aurora_thread_config_t *cfg);
void aurora_thread_discover(aurora_thread_stats_t *ts);

/* ---------------------------------------------------------------
 * Module-private state
 * --------------------------------------------------------------- */

static volatile int          g_proc_stop = 0;
static pthread_t             g_proc_tid;
static aurora_thread_stats_t *g_ts = NULL;
static uint32_t              g_interval_ms = 100;

/* ---------------------------------------------------------------
 * procfs stat parsing
 * --------------------------------------------------------------- */

/*
 * stat_fields — parsed subset of /proc/[pid]/task/[tid]/stat.
 *
 * The stat file has 52+ space-separated fields.
 * We read exactly the fields we need by counting tokens.
 *
 * Field indices (1-based, as in proc(5) man page):
 *   14: utime   (clock ticks)
 *   15: stime   (clock ticks)
 *   39: processor  (last CPU, 0-indexed)
 */
typedef struct {
    unsigned long utime;    /* user-mode CPU ticks */
    unsigned long stime;    /* kernel-mode CPU ticks */
    int           cpu;      /* last CPU (field 39) */
} proc_stat_t;

static int read_task_stat(pid_t pid, pid_t tid, proc_stat_t *out)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid, tid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    /* The comm field (2) may contain spaces and is enclosed in parentheses.
     * Skip past the closing ')' to avoid misparse. */
    int c;
    /* Skip to the closing ')' */
    while ((c = fgetc(f)) != EOF && c != ')')
        ;
    if (c == EOF) { fclose(f); return -1; }

    /* Now parse the remaining fields starting from field 3.
     * We need fields 14 (utime), 15 (stime), 39 (processor).
     * After ')' they are at positions 3..N.
     * utime  = field 14 → offset 11 after ')'
     * stime  = field 15 → offset 12 after ')'
     * cpu    = field 39 → offset 36 after ')'
     */
    unsigned long dummy_ul;
    long dummy_l;
    unsigned long long dummy_ull;
    int dummy_i;
    unsigned int dummy_ui;

    /* Fields 3-13 (11 fields to skip) */
    /* 3: state */          char state; if (fscanf(f, " %c", &state) != 1) { fclose(f); return -1; }
    /* 4: ppid */           if (fscanf(f, " %d", &dummy_i) != 1) { fclose(f); return -1; }
    /* 5: pgrp */           if (fscanf(f, " %d", &dummy_i) != 1) { fclose(f); return -1; }
    /* 6: session */        if (fscanf(f, " %d", &dummy_i) != 1) { fclose(f); return -1; }
    /* 7: tty_nr */         if (fscanf(f, " %d", &dummy_i) != 1) { fclose(f); return -1; }
    /* 8: tpgid */          if (fscanf(f, " %d", &dummy_i) != 1) { fclose(f); return -1; }
    /* 9: flags */          if (fscanf(f, " %u", &dummy_ui) != 1) { fclose(f); return -1; }
    /* 10: minflt */        if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 11: cminflt */       if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 12: majflt */        if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 13: cmajflt */       if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 14: utime */         if (fscanf(f, " %lu", &out->utime) != 1) { fclose(f); return -1; }
    /* 15: stime */         if (fscanf(f, " %lu", &out->stime) != 1) { fclose(f); return -1; }
    /* 16: cutime */        if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 17: cstime */        if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 18: priority */      if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 19: nice */          if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 20: num_threads */   if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 21: itrealvalue */   if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 22: starttime */     if (fscanf(f, " %llu", &dummy_ull) != 1) { fclose(f); return -1; }
    /* 23: vsize */         if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 24: rss */           if (fscanf(f, " %ld", &dummy_l) != 1) { fclose(f); return -1; }
    /* 25: rsslim */        if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 26-38: memory addrs (13 fields) */
    for (int skip = 0; skip < 13; skip++)
        if (fscanf(f, " %lu", &dummy_ul) != 1) { fclose(f); return -1; }
    /* 39: processor */     if (fscanf(f, " %d", &out->cpu) != 1) { fclose(f); return -1; }

    fclose(f);
    return 0;
}

/*
 * Read voluntary and nonvoluntary context switches from /proc/status.
 * Returns 0 on success.
 */
static int read_task_ctxsw(pid_t pid, pid_t tid,
                            uint64_t *vol_sw, uint64_t *nonvol_sw)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pid, tid);

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    *vol_sw    = 0;
    *nonvol_sw = 0;
    int found  = 0;
    char line[128];

    while (fgets(line, sizeof(line), f) && found < 2) {
        unsigned long long val;
        if (sscanf(line, "voluntary_ctxt_switches: %llu", &val) == 1) {
            *vol_sw = (uint64_t)val;
            found++;
        } else if (sscanf(line, "nonvoluntary_ctxt_switches: %llu", &val) == 1) {
            *nonvol_sw = (uint64_t)val;
            found++;
        }
    }
    fclose(f);
    return (found == 2) ? 0 : -1;
}

/* ---------------------------------------------------------------
 * Per-thread collection tick
 * --------------------------------------------------------------- */

static long g_hz = 0; /* Cached sysconf(_SC_CLK_TCK) */

static void collect_thread_metrics(aurora_thread_metrics_t *m, pid_t pid)
{
    if (!m->valid)
        return;

    proc_stat_t st = {0};
    if (read_task_stat(pid, m->tid, &st) != 0)
        return; /* Thread may have exited; will be cleaned up next discovery */

    /* CPU time delta in nanoseconds */
    unsigned long total_ticks = st.utime + st.stime;
    uint64_t delta_ticks = 0;
    if (total_ticks >= (unsigned long)(m->_prev_utime_ticks + m->_prev_stime_ticks))
        delta_ticks = total_ticks - (m->_prev_utime_ticks + m->_prev_stime_ticks);

    uint64_t delta_ns = (g_hz > 0) ? (delta_ticks * 1000000000ULL / (uint64_t)g_hz) : 0;

    m->_prev_utime_ticks = st.utime;
    m->_prev_stime_ticks = st.stime;

    /* Record as a histogram quantum if non-zero */
    if (delta_ns > 0)
        aurora_thread_record_quantum(m, delta_ns);

    /* CPU migration */
    if (m->_prev_cpu >= 0 && st.cpu != m->_prev_cpu)
        m->cpu_migrations++;
    m->cpu_id   = st.cpu;
    m->_prev_cpu = st.cpu;

    /* Context switches (cumulative deltas) */
    uint64_t vol_sw = 0, nonvol_sw = 0;
    if (read_task_ctxsw(pid, m->tid, &vol_sw, &nonvol_sw) == 0) {
        if (vol_sw >= m->_prev_vol_sw)
            m->voluntary_ctxsw += vol_sw - m->_prev_vol_sw;
        if (nonvol_sw >= m->_prev_nonvol_sw)
            m->involuntary_ctxsw += nonvol_sw - m->_prev_nonvol_sw;
        m->_prev_vol_sw    = vol_sw;
        m->_prev_nonvol_sw = nonvol_sw;

        /* Approximate IRQ count as nonvoluntary (preemption) delta */
        m->irq_count = m->involuntary_ctxsw;
    }

    /* Update timestamp */
    struct timespec ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_now);
    m->timestamp_ns = (uint64_t)ts_now.tv_sec * 1000000000ULL + (uint64_t)ts_now.tv_nsec;
}

/* ---------------------------------------------------------------
 * One collection step (OCUDU: driven synchronously by the AURORA
 * publisher once per interval instead of a free-running thread).
 * --------------------------------------------------------------- */
void aurora_procfs_collect_once(void)
{
    if (!g_ts || g_proc_stop)
        return;

    pid_t pid = getpid();

    /* Discover new threads */
    aurora_thread_discover(g_ts);

    /* Collect metrics for all registered threads */
    uint32_t n = atomic_load_explicit(&g_ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < AURORA_MAX_MONITORED_THREADS; i++)
        collect_thread_metrics(&g_ts->threads[i], pid);

    /* Recompute derived statistics */
    aurora_thread_compute_all_stats(g_ts);
}

/* ---------------------------------------------------------------
 * Public API called from aurora_thread.c
 * --------------------------------------------------------------- */

int aurora_procfs_start(aurora_thread_stats_t *ts, uint32_t interval_ms)
{
    g_ts          = ts;
    g_interval_ms = interval_ms;
    g_proc_stop   = 0;
    g_hz          = sysconf(_SC_CLK_TCK);
    if (g_hz <= 0)
        g_hz = 100; /* Safe default */
    return 0;
}

void aurora_procfs_stop(void)
{
    g_proc_stop = 1;
}
