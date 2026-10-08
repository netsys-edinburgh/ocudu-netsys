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
 * aurora_thread.c — Thread metrics subsystem coordinator.
 *
 * Implements aurora_thread_init() which:
 *   1. Loads whitelist from config
 *   2. Tries eBPF backend (if compiled in and available)
 *   3. Falls back to procfs polling
 *
 * Implements aurora_thread_shutdown() to cleanly stop the collector.
 */

#include "aurora_thread.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* External: aurora_thread_registry.c */
void aurora_thread_registry_init(const aurora_thread_config_t *cfg);

/* External: aurora_thread_proc.c */
int  aurora_procfs_start(aurora_thread_stats_t *ts, uint32_t interval_ms);
void aurora_procfs_stop(void);

#ifdef AURORA_THREAD_EBPF_ENABLED
/* External: aurora_thread_ebpf.c */
int  aurora_ebpf_start(aurora_thread_stats_t *ts, uint32_t interval_ms);
void aurora_ebpf_stop(void);
#endif

/* ---------------------------------------------------------------
 * Module-private state
 * --------------------------------------------------------------- */
static aurora_thread_config_t g_cfg;
static aurora_thread_stats_t *g_ts = NULL;
static int                    g_ebpf_active = 0;
static int                    g_initialized = 0;

/* Configured collection interval — passed through from aurora_config_t */
static uint32_t g_interval_ms = 100;

/* ---------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------- */

int aurora_thread_init(const aurora_thread_config_t *cfg, aurora_thread_stats_t *ts)
{
    if (!cfg || !ts) {
        fprintf(stderr, "AURORA thread: NULL argument to aurora_thread_init\n");
        return -1;
    }

    if (!cfg->enabled) {
        printf("AURORA thread: thread metrics collection disabled\n");
        return 0;
    }

    g_cfg = *cfg;
    g_ts  = ts;

    /* Initialise shared memory region */
    atomic_store_explicit(&g_ts->num_threads, 0, memory_order_relaxed);
    memset(g_ts->threads, 0, sizeof(g_ts->threads));

    /* Initialise whitelist and process name cache */
    aurora_thread_registry_init(cfg);

    /* Try eBPF backend first if requested or auto */
    g_ebpf_active = 0;

#ifdef AURORA_THREAD_EBPF_ENABLED
    if (cfg->backend == AURORA_THREAD_BACKEND_EBPF ||
        cfg->backend == AURORA_THREAD_BACKEND_AUTO) {
        int ret = aurora_ebpf_start(ts, g_interval_ms);
        if (ret == 0) {
            printf("AURORA thread: eBPF backend started\n");
            g_ebpf_active = 1;
            g_initialized = 1;
            return 0;
        }
        if (cfg->backend == AURORA_THREAD_BACKEND_EBPF) {
            fprintf(stderr, "AURORA thread: eBPF forced but unavailable\n");
            return -1;
        }
        printf("AURORA thread: eBPF unavailable, falling back to procfs\n");
    }
#else
    if (cfg->backend == AURORA_THREAD_BACKEND_EBPF) {
        fprintf(stderr, "AURORA thread: eBPF support not compiled in\n");
        return -1;
    }
#endif

    /* procfs fallback (or explicit procfs backend) */
    int ret = aurora_procfs_start(ts, g_interval_ms);
    if (ret != 0) {
        fprintf(stderr, "AURORA thread: failed to start procfs collector\n");
        return -1;
    }
    printf("AURORA thread: procfs backend started\n");
    g_initialized = 1;
    return 0;
}

void aurora_thread_shutdown(void)
{
    if (!g_initialized)
        return;

#ifdef AURORA_THREAD_EBPF_ENABLED
    if (g_ebpf_active)
        aurora_ebpf_stop();
    else
#endif
        aurora_procfs_stop();

    g_initialized = 0;
}

/* Expose interval setter so aurora.c can pass the configured interval */
void aurora_thread_set_interval_ms(uint32_t interval_ms)
{
    g_interval_ms = interval_ms;
}
