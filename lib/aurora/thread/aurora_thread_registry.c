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
 * aurora_thread_registry.c — Thread whitelist matching and slot management.
 *
 * Thread discovery is done by scanning /proc/self/task/ each collection
 * interval (from aurora_thread_proc.c).  This module provides:
 *   - Whitelist loading and name-pattern matching
 *   - Slot allocation in aurora_thread_stats_t
 *   - /proc/self/task/ scanner (enumerates all live kernel TIDs)
 */

#include "aurora_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/types.h>
#include <time.h>

/* ---------------------------------------------------------------
 * Module-private state
 * --------------------------------------------------------------- */

/* Whitelist patterns loaded from config (NULL-terminated array of strings) */
#define AURORA_MAX_WHITELIST_PATTERNS  64
static char whitelist_patterns[AURORA_MAX_WHITELIST_PATTERNS][AURORA_THREAD_NAME_LEN + 1];
static int  whitelist_count = 0;

/* Cached process name (read once at init) */
static char g_process_name[AURORA_PROC_NAME_LEN] = {0};

/* ---------------------------------------------------------------
 * Whitelist management
 * --------------------------------------------------------------- */

/*
 * aurora_thread_load_whitelist — parse comma-separated pattern string.
 *
 * Patterns may include trailing '*' as a prefix wildcard.
 * Example: "L1_rx_thread,L1_tx_thread,Tpool*,GTPrx_*"
 */
static void aurora_thread_load_whitelist(const char *whitelist_str)
{
    whitelist_count = 0;
    if (!whitelist_str || whitelist_str[0] == '\0')
        return;

    char buf[AURORA_THREAD_WHITELIST_MAX];
    strncpy(buf, whitelist_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *saveptr = NULL;
    char *token = strtok_r(buf, ",", &saveptr);
    while (token && whitelist_count < AURORA_MAX_WHITELIST_PATTERNS) {
        /* Trim leading spaces */
        while (*token == ' ') token++;
        if (*token == '\0') {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }
        strncpy(whitelist_patterns[whitelist_count], token, AURORA_THREAD_NAME_LEN);
        whitelist_patterns[whitelist_count][AURORA_THREAD_NAME_LEN] = '\0';
        whitelist_count++;
        token = strtok_r(NULL, ",", &saveptr);
    }
}

/*
 * aurora_thread_name_matches — return 1 if name matches any whitelist pattern.
 *
 * Pattern matching rules:
 *   - Exact match: "L1_rx_thread" matches only "L1_rx_thread"
 *   - Trailing '*': "Tpool*" matches "Tpool0_2", "Tpool1_-1", etc.
 *   - Empty whitelist (whitelist_count == 0): match all threads
 */
int aurora_thread_name_matches(const char *name)
{
    if (!name || name[0] == '\0')
        return 0;

    /* Empty whitelist = monitor all named threads */
    if (whitelist_count == 0)
        return 1;

    for (int i = 0; i < whitelist_count; i++) {
        const char *pat = whitelist_patterns[i];
        size_t pat_len  = strlen(pat);

        if (pat_len == 0)
            continue;

        if (pat[pat_len - 1] == '*') {
            /* Prefix match */
            if (strncmp(name, pat, pat_len - 1) == 0)
                return 1;
        } else {
            /* Exact match */
            if (strcmp(name, pat) == 0)
                return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------
 * Slot management
 * --------------------------------------------------------------- */

/*
 * aurora_thread_find_slot — linear search for TID.
 * Returns index in ts->threads[] or -1.
 */
int aurora_thread_find_slot(aurora_thread_stats_t *ts, pid_t tid)
{
    uint32_t n = atomic_load_explicit(&ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < AURORA_MAX_MONITORED_THREADS; i++) {
        if (ts->threads[i].valid && ts->threads[i].tid == tid)
            return (int)i;
    }
    return -1;
}

/*
 * aurora_thread_alloc_slot — find or create a slot for (tid, name).
 * Returns index or -1 if table is full.
 */
static int aurora_thread_alloc_slot(aurora_thread_stats_t *ts, pid_t tid, const char *name)
{
    /* Reuse existing slot if TID already registered */
    int existing = aurora_thread_find_slot(ts, tid);
    if (existing >= 0)
        return existing;

    /* Find a free slot */
    uint32_t n = atomic_load_explicit(&ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < AURORA_MAX_MONITORED_THREADS; i++) {
        if (!ts->threads[i].valid) {
            aurora_thread_metrics_t *m = &ts->threads[i];
            memset(m, 0, sizeof(*m));
            m->tid = tid;
            m->pid = getpid();
            memcpy(m->thread_name, name, AURORA_THREAD_NAME_LEN - 1);
            m->thread_name[AURORA_THREAD_NAME_LEN - 1] = '\0';
            memcpy(m->process_name, g_process_name, AURORA_PROC_NAME_LEN - 1);
            m->process_name[AURORA_PROC_NAME_LEN - 1] = '\0';
            m->valid = 1;

            /* Update num_threads counter */
            if (i >= n)
                atomic_store_explicit(&ts->num_threads, i + 1, memory_order_relaxed);

            return (int)i;
        }
    }
    return -1; /* Table full */
}

/*
 * aurora_thread_free_stale_slots — mark slots for TIDs that no longer exist.
 *
 * Called each discovery cycle.  Checks /proc/self/task/{tid} existence.
 */
static void aurora_thread_free_stale_slots(aurora_thread_stats_t *ts)
{
    uint32_t n = atomic_load_explicit(&ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < AURORA_MAX_MONITORED_THREADS; i++) {
        if (!ts->threads[i].valid)
            continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/task/%d", getpid(), ts->threads[i].tid);
        if (access(path, F_OK) != 0) {
            /* Thread is gone — free slot */
            ts->threads[i].valid = 0;
            ts->threads[i].tid   = 0;
        }
    }
}

/* ---------------------------------------------------------------
 * Thread discovery via /proc/self/task/
 * --------------------------------------------------------------- */

/*
 * Read thread name from /proc/self/task/{tid}/comm.
 * Returns 1 on success, 0 on failure.
 */
static int read_thread_comm(pid_t tid, char *out, size_t out_len)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/comm", getpid(), tid);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(out, (int)out_len, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    /* Strip trailing newline */
    size_t len = strlen(out);
    if (len > 0 && out[len - 1] == '\n')
        out[len - 1] = '\0';
    return 1;
}

/*
 * aurora_thread_discover — scan /proc/self/task/ and register newly
 * visible threads whose names match the whitelist.
 *
 * Called each collection interval from aurora_thread_proc.c.
 */
void aurora_thread_discover(aurora_thread_stats_t *ts)
{
    /* First, free slots for threads that have exited */
    aurora_thread_free_stale_slots(ts);

    char task_dir[32];
    snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", getpid());

    DIR *d = opendir(task_dir);
    if (!d)
        return;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        /* Skip non-numeric entries (. and ..) */
        if (!isdigit((unsigned char)entry->d_name[0]))
            continue;

        pid_t tid = (pid_t)atoi(entry->d_name);
        if (tid <= 0)
            continue;

        /* Skip if already registered */
        if (aurora_thread_find_slot(ts, tid) >= 0)
            continue;

        /* Read thread name */
        char thread_name[AURORA_THREAD_NAME_LEN] = {0};
        if (!read_thread_comm(tid, thread_name, sizeof(thread_name)))
            continue;

        /* Apply whitelist filter */
        if (!aurora_thread_name_matches(thread_name))
            continue;

        /* Allocate slot */
        aurora_thread_alloc_slot(ts, tid, thread_name);
    }
    closedir(d);
}

/* ---------------------------------------------------------------
 * Initialisation
 * --------------------------------------------------------------- */

void aurora_thread_registry_init(const aurora_thread_config_t *cfg)
{
    /* Cache process name */
    FILE *f = fopen("/proc/self/comm", "r");
    if (f) {
        if (fgets(g_process_name, sizeof(g_process_name), f)) {
            size_t len = strlen(g_process_name);
            if (len > 0 && g_process_name[len - 1] == '\n')
                g_process_name[len - 1] = '\0';
        }
        fclose(f);
    } else {
        strncpy(g_process_name, "unknown", sizeof(g_process_name) - 1);
    }

    /* Parse whitelist */
    aurora_thread_load_whitelist(cfg->whitelist);
}
