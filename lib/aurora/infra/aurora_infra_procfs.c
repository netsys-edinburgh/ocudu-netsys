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
 */

/* Needed for setns()/CLONE_NEWNET (container-netns carrier_changes read). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/*
 * aurora_infra_procfs.c — procfs/sysfs/cgroup filesystem backend
 *                          for infrastructure metrics collection.
 *
 * All functions read from filesystem paths and are safe to call
 * when eBPF is unavailable.  Host paths are prefixed with
 * cfg->host_proc_prefix / cfg->host_sys_prefix when running
 * inside a sidecar container with bind-mounted host filesystems.
 */

#include "aurora_infra.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/time.h>
#include <sys/statvfs.h>
#include <inttypes.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Read entire small file into buffer.  Returns bytes read or -1. */
static int read_file(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    buf[n] = '\0';
    fclose(f);
    return (int)n;
}

/* Read a single uint64 from a file. Returns 0 on success. */
static int read_u64(const char *path, uint64_t *out)
{
    char buf[64];
    if (read_file(path, buf, sizeof(buf)) < 0) return -1;
    *out = strtoull(buf, NULL, 10);
    return 0;
}

/* Read a single int32 from a file. */
static int read_i32(const char *path, int32_t *out)
{
    char buf[64];
    if (read_file(path, buf, sizeof(buf)) < 0) return -1;
    *out = (int32_t)atoi(buf);
    return 0;
}

/* Build path: prefix + suffix. */
static void build_path(char *dst, size_t dstsz, const char *prefix, const char *suffix)
{
    if (prefix[0])
        snprintf(dst, dstsz, "%s%s", prefix, suffix);
    else
        snprintf(dst, dstsz, "%s", suffix);
}

/* Parse PSI line: "some avg10=X avg60=Y avg300=Z total=T" */
static void parse_psi_line(const char *line, float *avg10, float *avg60, float *avg300)
{
    const char *p;
    if ((p = strstr(line, "avg10=")))  *avg10  = strtof(p + 6, NULL);
    if ((p = strstr(line, "avg60=")))  *avg60  = strtof(p + 6, NULL);
    if ((p = strstr(line, "avg300="))) *avg300 = strtof(p + 7, NULL);
}

/* ---------------------------------------------------------------
 * State for delta computation (static, one collector per process)
 * --------------------------------------------------------------- */
static struct {
    uint64_t prev_cpu_usage_us;
    uint64_t prev_timestamp_us;
    uint64_t prev_vol_sw;
    uint64_t prev_nonvol_sw;
    uint64_t prev_pgfault;
    uint64_t prev_pgmajfault;
    /* host CPU /proc/stat */
    uint64_t prev_cpu_user, prev_cpu_nice, prev_cpu_system;
    uint64_t prev_cpu_idle, prev_cpu_iowait, prev_cpu_irq, prev_cpu_softirq;
    uint64_t prev_cpu_steal;
    /* host softirqs */
    uint64_t prev_softirq_net_rx, prev_softirq_net_tx;
    uint64_t prev_softirq_timer, prev_softirq_sched;
    /* host disk */
    uint64_t prev_disk_read_ios, prev_disk_write_ios;
    uint64_t prev_disk_read_sectors, prev_disk_write_sectors;
    uint64_t prev_disk_io_time_ms;
    uint64_t prev_disk_timestamp_us;
    /* container net */
    uint64_t prev_net_rx_bytes, prev_net_tx_bytes;
    uint64_t prev_net_rx_packets, prev_net_tx_packets;
    uint64_t prev_tcp_retransmits;
    /* container I/O */
    uint64_t prev_io_read_bytes, prev_io_write_bytes;
    uint64_t prev_io_read_ops, prev_io_write_ops;
    uint64_t prev_io_wait_us;
    int container_cpu_initialized;
    int container_mem_initialized;
    int container_net_initialized;
    int container_io_initialized;
    int host_cpu_initialized;
    int host_disk_initialized;
} g_prev;

/* ===============================================================
 * CONTAINER METRICS COLLECTION
 * =============================================================== */

/* ---------------------------------------------------------------
 * Container CPU — from cgroup v2 cpu.stat
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_container_cpu(const aurora_infra_config_t *cfg,
                                              aurora_container_cpu_t *out)
{
    char path[512];
    char buf[2048];

    memset(out, 0, sizeof(*out));

    /* Read cpu.stat from cgroup */
    snprintf(path, sizeof(path), "%s/cpu.stat", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) < 0) {
        /* Fallback: try /proc/<pid>/stat */
        if (cfg->target_pid > 0) {
            char proc_path[600];
            snprintf(proc_path, sizeof(proc_path), "%s/proc/%d/stat",
                     cfg->host_proc_prefix, cfg->target_pid);
            if (read_file(proc_path, buf, sizeof(buf)) >= 0) {
                /* Parse fields 14 (utime) and 15 (stime) in clock ticks */
                char *p = buf;
                int field = 0;
                uint64_t utime = 0, stime = 0;
                /* Skip past the comm field (contains parentheses) */
                p = strchr(p, ')');
                if (p) { p += 2; field = 2; }
                while (p && field < 15) {
                    if (field == 13) utime = strtoull(p, &p, 10);
                    else if (field == 14) stime = strtoull(p, &p, 10);
                    else strtoull(p, &p, 10);
                    field++;
                    while (*p == ' ') p++;
                }
                long ticks_per_sec = sysconf(_SC_CLK_TCK);
                if (ticks_per_sec <= 0) ticks_per_sec = 100;
                out->cpu_usage_us = (utime + stime) * 1000000ULL / (uint64_t)ticks_per_sec;
                out->cpu_user_us = utime * 1000000ULL / (uint64_t)ticks_per_sec;
                out->cpu_system_us = stime * 1000000ULL / (uint64_t)ticks_per_sec;
            }
        }
    } else {
        /* Parse cgroup cpu.stat */
        char *line = buf;
        while (line && *line) {
            if (strncmp(line, "usage_usec ", 11) == 0)
                out->cpu_usage_us = strtoull(line + 11, NULL, 10);
            else if (strncmp(line, "user_usec ", 10) == 0)
                out->cpu_user_us = strtoull(line + 10, NULL, 10);
            else if (strncmp(line, "system_usec ", 12) == 0)
                out->cpu_system_us = strtoull(line + 12, NULL, 10);
            else if (strncmp(line, "nr_throttled ", 13) == 0)
                out->cpu_throttle_count = (uint32_t)strtoul(line + 13, NULL, 10);
            else if (strncmp(line, "throttled_usec ", 15) == 0)
                out->cpu_throttled_us = strtoull(line + 15, NULL, 10);
            else if (strncmp(line, "nr_periods ", 11) == 0)
                out->cpu_nr_periods = (uint32_t)strtoul(line + 11, NULL, 10);
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* Compute CPU usage percentage from delta */
    uint64_t now = now_us();
    if (g_prev.container_cpu_initialized && g_prev.prev_timestamp_us > 0) {
        uint64_t dt = now - g_prev.prev_timestamp_us;
        if (dt > 0) {
            uint64_t cpu_delta = out->cpu_usage_us - g_prev.prev_cpu_usage_us;
            out->cpu_usage_pct = (double)cpu_delta / (double)dt * 100.0;
        }
    }

    /* Context switches from /proc/<pid>/status */
    if (cfg->target_pid > 0) {
        snprintf(path, sizeof(path), "%s/proc/%d/status",
                 cfg->host_proc_prefix, cfg->target_pid);
        if (read_file(path, buf, sizeof(buf)) >= 0) {
            char *p;
            uint64_t vol = 0, nonvol = 0;
            if ((p = strstr(buf, "voluntary_ctxt_switches:")))
                vol = strtoull(p + 24, NULL, 10);
            if ((p = strstr(buf, "nonvoluntary_ctxt_switches:")))
                nonvol = strtoull(p + 27, NULL, 10);

            if (g_prev.container_cpu_initialized) {
                out->voluntary_ctxsw = vol - g_prev.prev_vol_sw;
                out->involuntary_ctxsw = nonvol - g_prev.prev_nonvol_sw;
            }
            g_prev.prev_vol_sw = vol;
            g_prev.prev_nonvol_sw = nonvol;
        }
    }

    g_prev.prev_cpu_usage_us = out->cpu_usage_us;
    g_prev.prev_timestamp_us = now;
    if (!g_prev.container_cpu_initialized) g_prev.container_cpu_initialized = 1;

    return 0;
}

/* ---------------------------------------------------------------
 * Container Memory — from cgroup v2 memory.*
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_container_mem(const aurora_infra_config_t *cfg,
                                              aurora_container_mem_t *out)
{
    char path[512], buf[4096];
    memset(out, 0, sizeof(*out));

    /* memory.current */
    snprintf(path, sizeof(path), "%s/memory.current", cfg->cgroup_path);
    read_u64(path, &out->mem_current_bytes);

    /* memory.max */
    snprintf(path, sizeof(path), "%s/memory.max", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        if (strncmp(buf, "max", 3) == 0)
            out->mem_limit_bytes = UINT64_MAX;
        else
            out->mem_limit_bytes = strtoull(buf, NULL, 10);
    }

    /* memory.swap.current */
    snprintf(path, sizeof(path), "%s/memory.swap.current", cfg->cgroup_path);
    read_u64(path, &out->mem_swap_bytes);

    /* memory.stat for RSS and cache */
    snprintf(path, sizeof(path), "%s/memory.stat", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        uint64_t pgfault = 0, pgmajfault = 0;
        while (line && *line) {
            if (strncmp(line, "anon ", 5) == 0)
                out->mem_rss_bytes = strtoull(line + 5, NULL, 10);
            else if (strncmp(line, "file ", 5) == 0)
                out->mem_cache_bytes = strtoull(line + 5, NULL, 10);
            else if (strncmp(line, "pgfault ", 8) == 0)
                pgfault = strtoull(line + 8, NULL, 10);
            else if (strncmp(line, "pgmajfault ", 11) == 0)
                pgmajfault = strtoull(line + 11, NULL, 10);
            line = strchr(line, '\n');
            if (line) line++;
        }
        if (g_prev.container_mem_initialized) {
            out->mem_pgfault = pgfault - g_prev.prev_pgfault;
            out->mem_pgmajfault = pgmajfault - g_prev.prev_pgmajfault;
        }
        g_prev.prev_pgfault = pgfault;
        g_prev.prev_pgmajfault = pgmajfault;
        g_prev.container_mem_initialized = 1;
    }

    /* memory.events for OOM kills */
    snprintf(path, sizeof(path), "%s/memory.events", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *p = strstr(buf, "oom_kill ");
        if (p) out->mem_oom_kill_count = (uint32_t)strtoul(p + 9, NULL, 10);
    }

    /* Usage percentage */
    if (out->mem_limit_bytes > 0 && out->mem_limit_bytes != UINT64_MAX)
        out->mem_usage_pct = (double)out->mem_current_bytes / (double)out->mem_limit_bytes * 100.0;

    /* memory.pressure (PSI) */
    snprintf(path, sizeof(path), "%s/memory.pressure", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        while (line && *line) {
            if (strncmp(line, "some ", 5) == 0)
                parse_psi_line(line, &out->mem_psi_some_avg10,
                               &out->mem_psi_some_avg60, &out->mem_psi_some_avg300);
            else if (strncmp(line, "full ", 5) == 0)
                parse_psi_line(line, &out->mem_psi_full_avg10,
                               &out->mem_psi_full_avg60, &out->mem_psi_full_avg300);
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Container Network — from /proc/<pid>/net/dev + /proc/<pid>/net/snmp
 * --------------------------------------------------------------- */
#define CONTAINER_NET_MAX_IFACES 8

/* Sum carrier_changes across the given interface names as seen inside the
 * TARGET process's own network namespace.
 *
 * /sys/class/net/<iface>/carrier_changes is NOT auto-virtualized per-pid
 * the way /proc/<pid>/net/dev is -- it always reflects the CALLING task's
 * *current* netns, not the target's. So unlike every other read in this
 * file, this one briefly calls setns() to join the target's netns, reads,
 * then restores this thread's own netns before returning.
 *
 * This runs on the single shared infra collector thread (see
 * infra_collector_thread() in aurora_infra.c), which polls host-level
 * metrics right after container metrics on every tick. If the restore
 * step below were ever skipped or silently failed, EVERY subsequent
 * host-level /proc and /sys read for the rest of the process's life would
 * silently read the CONTAINER's namespace instead of the host's -- a
 * severe, hard-to-notice correctness bug. So: open our own netns fd
 * FIRST (before touching anything), and always attempt the restore on
 * every exit path, including early-return-on-error ones; if the restore
 * itself fails, this is treated as fatal (abort) rather than continuing
 * in an unknown, unrecoverable namespace state.
 *
 * Returns the summed count (0 if nothing could be read -- e.g. missing
 * CAP_SYS_ADMIN, target process gone, or on kernels where the sysfs file
 * doesn't exist). Never fails silently into a stuck wrong-namespace state.
 */
static uint64_t read_container_carrier_changes(pid_t target_pid,
                                               char ifaces[][32],
                                               int n_ifaces)
{
    if (target_pid <= 0 || n_ifaces <= 0)
        return 0;

    char self_ns_path[64];
    char target_ns_path[64];
    snprintf(self_ns_path, sizeof(self_ns_path), "/proc/self/ns/net");
    snprintf(target_ns_path, sizeof(target_ns_path), "/proc/%d/ns/net", target_pid);

    int self_fd = open(self_ns_path, O_RDONLY);
    if (self_fd < 0)
        return 0; /* can't even save our own namespace -- don't risk switching */

    int target_fd = open(target_ns_path, O_RDONLY);
    if (target_fd < 0) {
        close(self_fd);
        return 0; /* target gone or inaccessible -- nothing attempted, nothing to restore */
    }

    uint64_t sum = 0;

    if (setns(target_fd, CLONE_NEWNET) == 0) {
        /* Now running inside the container's netns: plain (unprefixed)
         * /sys/class/net/ correctly reflects ITS interfaces, regardless of
         * this process's own mount namespace -- sysfs net entries are
         * keyed by the calling task's *current* netns, not by any bind
         * mount. Do NOT use cfg->host_sys_prefix here. */
        for (int i = 0; i < n_ifaces; i++) {
            char sys_path[96];
            uint64_t v;
            /* %.31s: ifaces[i] is char[32] (31 chars + NUL) so this never
             * truncates real data -- just makes the bound explicit instead
             * of relying on GCC's escape analysis to track the array size
             * through this parameter, which it doesn't always do (was
             * flagged as a possible truncation by -Wformat-truncation
             * otherwise, even though the real max length here is 63). */
            snprintf(sys_path, sizeof(sys_path), "/sys/class/net/%.31s/carrier_changes", ifaces[i]);
            if (read_u64(sys_path, &v) == 0)
                sum += v;
        }

        /* MUST restore before returning, success or not. */
        if (setns(self_fd, CLONE_NEWNET) != 0) {
            fprintf(stderr,
                    "AURORA_INFRA: FATAL: failed to restore own network namespace "
                    "after reading container %d's carrier_changes -- aborting to avoid "
                    "silently corrupting all subsequent host-metric reads\n",
                    target_pid);
            abort();
        }
    }
    /* else: couldn't join target netns (e.g. no CAP_SYS_ADMIN) -- we never
     * left our own namespace, so there is nothing to restore. Fall through
     * and return 0. */

    close(target_fd);
    close(self_fd);
    return sum;
}

int aurora_infra_procfs_collect_container_net(const aurora_infra_config_t *cfg,
                                              aurora_container_net_t *out)
{
    char path[512], buf[4096];
    memset(out, 0, sizeof(*out));

    if (cfg->target_pid <= 0) return -1;

    /* /proc/<pid>/net/dev */
    snprintf(path, sizeof(path), "%s/proc/%d/net/dev",
             cfg->host_proc_prefix, cfg->target_pid);
    char seen_ifaces[CONTAINER_NET_MAX_IFACES][32];
    int n_seen_ifaces = 0;

    if (read_file(path, buf, sizeof(buf)) >= 0) {
        uint64_t rx_b = 0, rx_p = 0, rx_e = 0, rx_d = 0;
        uint64_t tx_b = 0, tx_p = 0, tx_e = 0, tx_d = 0;
        char *line = buf;
        int lineno = 0;
        while (line && *line) {
            lineno++;
            if (lineno > 2) {  /* skip header lines */
                char iface[32] = {0};
                uint64_t rb, rp, re, rd, _r1, _r2, _r3, _r4;
                uint64_t tb, tp, te, td;
                /* Format: iface: rx_bytes rx_pkts rx_errs rx_drop ... tx_bytes tx_pkts tx_errs tx_drop ... */
                char *colon = strchr(line, ':');
                if (colon) {
                    /* Extract interface name */
                    char *s = line;
                    while (*s == ' ') s++;
                    size_t nlen = (size_t)(colon - s);
                    if (nlen >= sizeof(iface)) nlen = sizeof(iface) - 1;
                    memcpy(iface, s, nlen);
                    iface[nlen] = '\0';

                    /* Skip loopback */
                    if (strcmp(iface, "lo") != 0) {
                        if (sscanf(colon + 1,
                                   " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                                   " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                                   " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
                                   &rb, &rp, &re, &rd, &_r1, &_r2, &_r3, &_r4,
                                   &tb, &tp, &te, &td) == 12) {
                            rx_b += rb; rx_p += rp; rx_e += re; rx_d += rd;
                            tx_b += tb; tx_p += tp; tx_e += te; tx_d += td;
                            /* Save first non-lo interface name */
                            if (out->iface_name[0] == '\0')
                                snprintf(out->iface_name, sizeof(out->iface_name), "%s", iface);
                            if (n_seen_ifaces < CONTAINER_NET_MAX_IFACES)
                                snprintf(seen_ifaces[n_seen_ifaces++], 32, "%s", iface);
                        }
                    }
                }
            }
            line = strchr(line, '\n');
            if (line) line++;
        }

        out->carrier_changes = read_container_carrier_changes(cfg->target_pid, seen_ifaces, n_seen_ifaces);

        /* Store as deltas if we have previous values */
        if (g_prev.container_net_initialized) {
            out->rx_bytes   = rx_b - g_prev.prev_net_rx_bytes;
            out->tx_bytes   = tx_b - g_prev.prev_net_tx_bytes;
            out->rx_packets = rx_p - g_prev.prev_net_rx_packets;
            out->tx_packets = tx_p - g_prev.prev_net_tx_packets;
        } else {
            out->rx_bytes = rx_b; out->tx_bytes = tx_b;
            out->rx_packets = rx_p; out->tx_packets = tx_p;
        }
        out->rx_errors = rx_e; out->tx_errors = tx_e;
        out->rx_drops = rx_d;  out->tx_drops = tx_d;

        g_prev.prev_net_rx_bytes = rx_b;
        g_prev.prev_net_tx_bytes = tx_b;
        g_prev.prev_net_rx_packets = rx_p;
        g_prev.prev_net_tx_packets = tx_p;
    }

    /* TCP stats from /proc/<pid>/net/snmp */
    snprintf(path, sizeof(path), "%s/proc/%d/net/snmp",
             cfg->host_proc_prefix, cfg->target_pid);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        /* Find "Tcp:" header line, then values line */
        char *tcp_hdr = strstr(buf, "Tcp:");
        if (tcp_hdr) {
            char *tcp_vals = strstr(tcp_hdr + 4, "\nTcp:");
            if (tcp_vals) {
                tcp_vals++; /* skip newline */
                /* Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails
                 *      EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts InCsumErrors */
                uint64_t vals[16] = {0};
                char *p = strchr(tcp_vals, ':');
                if (p) {
                    p++; /* skip ':' */
                    for (int i = 0; i < 16; i++) {
                        while (*p == ' ') p++;
                        vals[i] = strtoull(p, &p, 10);
                    }
                    /* ActiveOpens=4, CurrEstab=8, RetransSegs=11 (0-indexed) */
                    if (g_prev.container_net_initialized)
                        out->tcp_retransmits = vals[11] - g_prev.prev_tcp_retransmits;
                    else
                        out->tcp_retransmits = vals[11];
                    out->tcp_active_opens = vals[4];
                    out->tcp_curr_estab = vals[8];
                    g_prev.prev_tcp_retransmits = vals[11];
                }
            }
        }
    }

    g_prev.container_net_initialized = 1;

    return 0;
}

/* ---------------------------------------------------------------
 * Container Disk I/O — from cgroup v2 io.stat
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_container_io(const aurora_infra_config_t *cfg,
                                             aurora_container_io_t *out)
{
    char path[512], buf[4096];
    memset(out, 0, sizeof(*out));

    /* io.stat from cgroup */
    snprintf(path, sizeof(path), "%s/io.stat", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        /* Format per line: "major:minor rbytes=X wbytes=Y rios=Z wios=W ..." */
        uint64_t total_rb = 0, total_wb = 0, total_rio = 0, total_wio = 0;
        char *line = buf;
        while (line && *line) {
            char *p;
            if ((p = strstr(line, "rbytes=")))
                total_rb += strtoull(p + 7, NULL, 10);
            if ((p = strstr(line, "wbytes=")))
                total_wb += strtoull(p + 7, NULL, 10);
            if ((p = strstr(line, "rios=")))
                total_rio += strtoull(p + 5, NULL, 10);
            if ((p = strstr(line, "wios=")))
                total_wio += strtoull(p + 5, NULL, 10);
            line = strchr(line, '\n');
            if (line) line++;
        }

        if (g_prev.container_io_initialized) {
            out->io_read_bytes  = total_rb  - g_prev.prev_io_read_bytes;
            out->io_write_bytes = total_wb  - g_prev.prev_io_write_bytes;
            out->io_read_ops    = total_rio - g_prev.prev_io_read_ops;
            out->io_write_ops   = total_wio - g_prev.prev_io_write_ops;
        }
        g_prev.prev_io_read_bytes  = total_rb;
        g_prev.prev_io_write_bytes = total_wb;
        g_prev.prev_io_read_ops    = total_rio;
        g_prev.prev_io_write_ops   = total_wio;
    }

    /* I/O wait from /proc/<pid>/stat field 42 (delayacct_blkio_ticks) */
    if (cfg->target_pid > 0) {
        snprintf(path, sizeof(path), "%s/proc/%d/stat",
                 cfg->host_proc_prefix, cfg->target_pid);
        if (read_file(path, buf, sizeof(buf)) >= 0) {
            char *p = strchr(buf, ')');
            if (p) {
                p += 2;
                int field = 2;
                while (p && field < 41) {
                    strtoull(p, &p, 10);
                    field++;
                    while (*p == ' ') p++;
                }
                if (field == 41) {
                    uint64_t blkio_ticks = strtoull(p, NULL, 10);
                    long tps = sysconf(_SC_CLK_TCK);
                    if (tps <= 0) tps = 100;
                    uint64_t iowait_us = blkio_ticks * 1000000ULL / (uint64_t)tps;
                    if (g_prev.container_io_initialized)
                        out->io_wait_us = iowait_us - g_prev.prev_io_wait_us;
                    g_prev.prev_io_wait_us = iowait_us;
                }
            }
        }
    }

    g_prev.container_io_initialized = 1;

    return 0;
}

/* ---------------------------------------------------------------
 * Container Process/FD metrics
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_container_proc(const aurora_infra_config_t *cfg,
                                               aurora_container_proc_t *out)
{
    char path[512], buf[1024];
    memset(out, 0, sizeof(*out));

    if (cfg->target_pid <= 0) return -1;

    /* Open FDs: count entries in /proc/<pid>/fd */
    snprintf(path, sizeof(path), "%s/proc/%d/fd",
             cfg->host_proc_prefix, cfg->target_pid);
    DIR *d = opendir(path);
    if (d) {
        uint32_t count = 0;
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] != '.') count++;
        }
        closedir(d);
        out->open_fds = count;
    }

    /* FD limit from /proc/<pid>/limits */
    snprintf(path, sizeof(path), "%s/proc/%d/limits",
             cfg->host_proc_prefix, cfg->target_pid);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *p = strstr(buf, "Max open files");
        if (p) {
            p += 14;
            while (*p == ' ') p++;
            out->fd_limit = (uint32_t)strtoul(p, NULL, 10);
        }
    }

    /* Thread count from /proc/<pid>/status */
    snprintf(path, sizeof(path), "%s/proc/%d/status",
             cfg->host_proc_prefix, cfg->target_pid);
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *p = strstr(buf, "Threads:");
        if (p) out->thread_count = (uint32_t)strtoul(p + 8, NULL, 10);
    }

    /* Process count from cgroup pids.current */
    snprintf(path, sizeof(path), "%s/pids.current", cfg->cgroup_path);
    if (read_file(path, buf, sizeof(buf)) >= 0)
        out->process_count = (uint32_t)strtoul(buf, NULL, 10);

    return 0;
}

/* ===============================================================
 * HOST METRICS COLLECTION
 * =============================================================== */

/* ---------------------------------------------------------------
 * Host Thermal — from /sys/class/thermal/thermal_zone*
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_thermal(const aurora_infra_config_t *cfg,
                                             aurora_host_thermal_t *out)
{
    char base[256], path[512], buf[128];
    memset(out, 0, sizeof(*out));

    build_path(base, sizeof(base), cfg->host_sys_prefix, "/sys/class/thermal");

    for (int i = 0; i < AURORA_INFRA_MAX_THERMAL_ZONES; i++) {
        snprintf(path, sizeof(path), "%s/thermal_zone%d/temp", base, i);
        int32_t temp;
        if (read_i32(path, &temp) < 0) break;

        aurora_thermal_zone_t *z = &out->zones[out->num_zones];
        z->temp_millicelsius = temp;
        z->valid = 1;

        /* Zone type */
        snprintf(path, sizeof(path), "%s/thermal_zone%d/type", base, i);
        if (read_file(path, buf, sizeof(buf)) >= 0) {
            buf[strcspn(buf, "\n")] = '\0';
            snprintf(z->zone_type, sizeof(z->zone_type), "%.31s", buf);
        }

        /* Trip point 0 (throttle threshold) */
        snprintf(path, sizeof(path), "%s/thermal_zone%d/trip_point_0_temp", base, i);
        read_i32(path, &z->trip_point_0_mc);

        out->num_zones++;
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Host CPU — /proc/loadavg, /proc/stat, cpufreq, PSI
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_cpu(const aurora_infra_config_t *cfg,
                                         aurora_host_cpu_t *out)
{
    char path[512], buf[8192];
    memset(out, 0, sizeof(*out));

    /* /proc/loadavg */
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/loadavg");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        sscanf(buf, "%f %f %f %u/%u",
               &out->load_1min, &out->load_5min, &out->load_15min,
               &out->runnable_tasks, &out->total_tasks);
    }

    /* Per-CPU frequency */
    for (uint32_t c = 0; c < AURORA_INFRA_MAX_CPUS; c++) {
        snprintf(path, sizeof(path), "%s/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq",
                 cfg->host_sys_prefix, c);
        uint64_t freq_khz;
        if (read_u64(path, &freq_khz) < 0) break;
        out->cpu_freq_mhz[c] = (uint32_t)(freq_khz / 1000);
        out->num_cpus = c + 1;
    }

    /* /proc/stat — aggregate CPU line */
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/stat");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
        if (sscanf(buf, "cpu  %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                   " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
                   &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal) == 8) {
            if (g_prev.host_cpu_initialized) {
                uint64_t du = user - g_prev.prev_cpu_user;
                uint64_t dn = nice - g_prev.prev_cpu_nice;
                uint64_t ds = system - g_prev.prev_cpu_system;
                uint64_t di = idle - g_prev.prev_cpu_idle;
                uint64_t dw = iowait - g_prev.prev_cpu_iowait;
                uint64_t dq = irq - g_prev.prev_cpu_irq;
                uint64_t df = softirq - g_prev.prev_cpu_softirq;
                uint64_t dst = steal - g_prev.prev_cpu_steal;
                uint64_t total = du + dn + ds + di + dw + dq + df + dst;
                if (total > 0) {
                    out->cpu_user_pct    = (double)(du + dn) / (double)total * 100.0;
                    out->cpu_system_pct  = (double)ds / (double)total * 100.0;
                    out->cpu_iowait_pct  = (double)dw / (double)total * 100.0;
                    out->cpu_idle_pct    = (double)di / (double)total * 100.0;
                    out->cpu_irq_pct     = (double)dq / (double)total * 100.0;
                    out->cpu_softirq_pct = (double)df / (double)total * 100.0;
                }
            }
            g_prev.prev_cpu_user    = user;
            g_prev.prev_cpu_nice    = nice;
            g_prev.prev_cpu_system  = system;
            g_prev.prev_cpu_idle    = idle;
            g_prev.prev_cpu_iowait  = iowait;
            g_prev.prev_cpu_irq     = irq;
            g_prev.prev_cpu_softirq = softirq;
            g_prev.prev_cpu_steal   = steal;
        }
    }

    /* CPU PSI */
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/pressure/cpu");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        while (line && *line) {
            if (strncmp(line, "some ", 5) == 0)
                parse_psi_line(line, &out->cpu_psi_some_avg10,
                               &out->cpu_psi_some_avg60, &out->cpu_psi_some_avg300);
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* Softirqs. /proc/softirqs has one column per logical CPU, ~11 softirq
     * type rows -- on a high-core-count host (seen live: 224 CPUs, file
     * ~27KB) this overruns the shared 8KB `buf` used for every other file
     * in this function, truncating before later rows (NET_RX sits past
     * the cutoff here, so it reads as permanently 0 regardless of real
     * traffic; NET_TX sits just inside it but still gets its line cut
     * short, under-counting most CPUs). Use a dedicated, much larger
     * buffer sized for this file specifically rather than growing the
     * shared `buf` (which stays sized for the many small files it also
     * reads) -- comfortably covers several hundred CPUs. */
    static char softirq_buf[65536];
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/softirqs");
    if (read_file(path, softirq_buf, sizeof(softirq_buf)) >= 0) {
        char *line = softirq_buf;
        while (line && *line) {
            uint64_t sum = 0;
            char *name_end = strchr(line, ':');
            if (name_end) {
                char *p = name_end + 1;
                /* Sum across all CPUs */
                while (*p) {
                    while (*p == ' ') p++;
                    if (*p == '\n' || *p == '\0') break;
                    sum += strtoull(p, &p, 10);
                }
                /* Identify which softirq */
                /* Trim leading spaces from name */
                char *ns = line;
                while (*ns == ' ') ns++;
                if (strncmp(ns, "NET_RX", 6) == 0) {
                    if (g_prev.host_cpu_initialized)
                        out->softirq_net_rx = sum - g_prev.prev_softirq_net_rx;
                    g_prev.prev_softirq_net_rx = sum;
                } else if (strncmp(ns, "NET_TX", 6) == 0) {
                    if (g_prev.host_cpu_initialized)
                        out->softirq_net_tx = sum - g_prev.prev_softirq_net_tx;
                    g_prev.prev_softirq_net_tx = sum;
                } else if (strncmp(ns, "TIMER", 5) == 0 && !isalpha((unsigned char)ns[5])) {
                    if (g_prev.host_cpu_initialized)
                        out->softirq_timer = sum - g_prev.prev_softirq_timer;
                    g_prev.prev_softirq_timer = sum;
                } else if (strncmp(ns, "SCHED", 5) == 0 && !isalpha((unsigned char)ns[5])) {
                    if (g_prev.host_cpu_initialized)
                        out->softirq_sched = sum - g_prev.prev_softirq_sched;
                    g_prev.prev_softirq_sched = sum;
                }
            }
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    g_prev.host_cpu_initialized = 1;

    return 0;
}

/* ---------------------------------------------------------------
 * Host Memory — /proc/meminfo + PSI
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_mem(const aurora_infra_config_t *cfg,
                                         aurora_host_mem_t *out)
{
    char path[512], buf[4096];
    memset(out, 0, sizeof(*out));

    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/meminfo");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        while (line && *line) {
            uint64_t val;
            if (sscanf(line, "MemTotal: %" PRIu64 " kB", &val) == 1) out->mem_total_kb = val;
            else if (sscanf(line, "MemAvailable: %" PRIu64 " kB", &val) == 1) out->mem_available_kb = val;
            else if (sscanf(line, "MemFree: %" PRIu64 " kB", &val) == 1) out->mem_free_kb = val;
            else if (sscanf(line, "Buffers: %" PRIu64 " kB", &val) == 1) out->mem_buffers_kb = val;
            else if (sscanf(line, "Cached: %" PRIu64 " kB", &val) == 1) out->mem_cached_kb = val;
            else if (sscanf(line, "SwapTotal: %" PRIu64 " kB", &val) == 1) out->swap_total_kb = val;
            else if (sscanf(line, "SwapFree: %" PRIu64 " kB", &val) == 1) out->swap_free_kb = val;
            else if (sscanf(line, "HugePages_Total: %" PRIu64, &val) == 1) out->hugepages_total = val;
            else if (sscanf(line, "HugePages_Free: %" PRIu64, &val) == 1) out->hugepages_free = val;
            else if (sscanf(line, "Hugepagesize: %u kB", &out->hugepage_size_kb) == 1) { /* done */ }
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* Memory PSI */
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/pressure/memory");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        while (line && *line) {
            if (strncmp(line, "some ", 5) == 0)
                parse_psi_line(line, &out->mem_psi_some_avg10,
                               &out->mem_psi_some_avg60, &out->mem_psi_some_avg300);
            else if (strncmp(line, "full ", 5) == 0)
                parse_psi_line(line, &out->mem_psi_full_avg10,
                               &out->mem_psi_full_avg60, &out->mem_psi_full_avg300);
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Host Disk — /proc/diskstats + I/O PSI
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_disk(const aurora_infra_config_t *cfg,
                                          aurora_host_disk_t *out)
{
    char path[512], buf[8192];
    memset(out, 0, sizeof(*out));

    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/diskstats");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        /* Find first disk device (sda, vda, nvme0n1, etc.) — skip partitions */
        char *line = buf;
        while (line && *line) {
            unsigned int major, minor;
            char devname[64] = {0};
            uint64_t rd_ios, rd_merge, rd_sec, rd_ticks;
            uint64_t wr_ios, wr_merge, wr_sec, wr_ticks;
            uint64_t in_flight, io_ticks, time_in_queue;

            int n = sscanf(line,
                " %u %u %63s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                " %" PRIu64 " %" PRIu64 " %" PRIu64,
                &major, &minor, devname,
                &rd_ios, &rd_merge, &rd_sec, &rd_ticks,
                &wr_ios, &wr_merge, &wr_sec, &wr_ticks,
                &in_flight, &io_ticks, &time_in_queue);

            if (n >= 14) {
                /* Pick first real disk: sda, vda, nvme0n1 (not partitions like sda1) */
                int is_disk = 0;
                if ((strncmp(devname, "sd", 2) == 0 || strncmp(devname, "vd", 2) == 0) &&
                    strlen(devname) == 3)
                    is_disk = 1;
                if (strncmp(devname, "nvme", 4) == 0 && strstr(devname, "p") == NULL)
                    is_disk = 1;

                if (is_disk) {
                    out->io_in_flight = in_flight;
                    uint64_t now = now_us();
                    if (g_prev.host_disk_initialized) {
                        out->read_ios      = rd_ios - g_prev.prev_disk_read_ios;
                        out->write_ios     = wr_ios - g_prev.prev_disk_write_ios;
                        out->read_sectors  = rd_sec - g_prev.prev_disk_read_sectors;
                        out->write_sectors = wr_sec - g_prev.prev_disk_write_sectors;
                        out->io_time_ms    = io_ticks - g_prev.prev_disk_io_time_ms;
                        /* Utilization estimate */
                        uint64_t dt = now - g_prev.prev_disk_timestamp_us;
                        if (dt > 0)
                            out->disk_util_pct = (double)out->io_time_ms * 1000.0 / (double)dt * 100.0;
                    }
                    g_prev.prev_disk_read_ios     = rd_ios;
                    g_prev.prev_disk_write_ios    = wr_ios;
                    g_prev.prev_disk_read_sectors = rd_sec;
                    g_prev.prev_disk_write_sectors = wr_sec;
                    g_prev.prev_disk_io_time_ms   = io_ticks;
                    g_prev.prev_disk_timestamp_us = now;
                    g_prev.host_disk_initialized  = 1;
                    break;  /* Use first real disk only */
                }
            }
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* I/O PSI */
    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/pressure/io");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        while (line && *line) {
            if (strncmp(line, "some ", 5) == 0)
                parse_psi_line(line, &out->io_psi_some_avg10,
                               &out->io_psi_some_avg60, &out->io_psi_some_avg300);
            else if (strncmp(line, "full ", 5) == 0)
                parse_psi_line(line, &out->io_psi_full_avg10,
                               &out->io_psi_full_avg60, &out->io_psi_full_avg300);
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Host Network — /proc/net/dev (host namespace)
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_net(const aurora_infra_config_t *cfg,
                                         aurora_host_net_t *out)
{
    char path[512], buf[8192];
    memset(out, 0, sizeof(*out));

    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/net/dev");
    if (read_file(path, buf, sizeof(buf)) >= 0) {
        char *line = buf;
        int lineno = 0;
        while (line && *line && out->num_nics < AURORA_INFRA_MAX_NICS) {
            lineno++;
            if (lineno > 2) {
                char iface[32] = {0};
                char *colon = strchr(line, ':');
                if (colon) {
                    char *s = line;
                    while (*s == ' ') s++;
                    size_t nlen = (size_t)(colon - s);
                    if (nlen >= sizeof(iface)) nlen = sizeof(iface) - 1;
                    memcpy(iface, s, nlen);
                    iface[nlen] = '\0';

                    if (strcmp(iface, "lo") != 0) {
                        aurora_host_nic_t *nic = &out->nics[out->num_nics];
                        snprintf(nic->name, sizeof(nic->name), "%s", iface);
                        nic->valid = 1;
                        uint64_t _skip1, _skip2, _skip3, _skip4;
                        sscanf(colon + 1,
                               " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                               " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64
                               " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64,
                               &nic->rx_bytes, &nic->rx_packets, &nic->rx_errors, &nic->rx_drops,
                               &_skip1, &_skip2, &_skip3, &_skip4,
                               &nic->tx_bytes, &nic->tx_packets, &nic->tx_errors, &nic->tx_drops);

                        {
                            char carrier_path[576];
                            char sys_suffix[64];
                            snprintf(sys_suffix, sizeof(sys_suffix),
                                     "/sys/class/net/%s/carrier_changes", iface);
                            build_path(carrier_path, sizeof(carrier_path),
                                       cfg->host_sys_prefix, sys_suffix);
                            if (read_u64(carrier_path, &nic->carrier_changes) != 0)
                                nic->carrier_changes = 0;
                        }

                        out->num_nics++;
                    }
                }
            }
            line = strchr(line, '\n');
            if (line) line++;
        }
    }

    /* Conntrack */
    build_path(path, sizeof(path), cfg->host_proc_prefix,
               "/proc/sys/net/netfilter/nf_conntrack_count");
    {
        uint64_t v;
        if (read_u64(path, &v) == 0) out->conntrack_count = (uint32_t)v;
    }
    build_path(path, sizeof(path), cfg->host_proc_prefix,
               "/proc/sys/net/netfilter/nf_conntrack_max");
    {
        uint64_t v;
        if (read_u64(path, &v) == 0) out->conntrack_max = (uint32_t)v;
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Host Kernel — entropy, uptime
 * --------------------------------------------------------------- */
int aurora_infra_procfs_collect_host_kernel(const aurora_infra_config_t *cfg,
                                            aurora_host_kernel_t *out)
{
    char path[512], buf[128];
    memset(out, 0, sizeof(*out));

    build_path(path, sizeof(path), cfg->host_proc_prefix,
               "/proc/sys/kernel/random/entropy_avail");
    {
        uint64_t v;
        if (read_u64(path, &v) == 0) out->entropy_avail = (uint32_t)v;
    }

    build_path(path, sizeof(path), cfg->host_proc_prefix, "/proc/uptime");
    if (read_file(path, buf, sizeof(buf)) >= 0)
        out->uptime_sec = strtod(buf, NULL);

    return 0;
}

/* ===============================================================
 * GPU METRICS — nvidia-smi CSV or sysfs fallback
 * =============================================================== */
int aurora_infra_procfs_collect_gpu(const aurora_infra_config_t *cfg,
                                    aurora_gpu_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    out->timestamp_us = now_us();

    if (!cfg->gpu_enabled) return 0;

    /* Try nvidia-smi --query-gpu with CSV output */
    FILE *fp = popen(
        "nvidia-smi --query-gpu="
        "index,name,utilization.gpu,utilization.memory,"
        "memory.used,memory.total,"
        "temperature.gpu,power.draw,power.limit,"
        "clocks.current.graphics,clocks.current.memory,"
        "pcie.link.gen.current,pcie.link.width.current,"
        "ecc.errors.corrected.volatile.total,"
        "ecc.errors.uncorrected.volatile.total"
        " --format=csv,noheader,nounits 2>/dev/null", "r");

    if (!fp) return -1;

    char line[512];
    while (fgets(line, sizeof(line), fp) && out->num_gpus < AURORA_INFRA_MAX_GPUS) {
        aurora_gpu_metrics_t *g = &out->gpus[out->num_gpus];
        memset(g, 0, sizeof(*g));

        /* Parse CSV: index, name, gpu_util, mem_util, mem_used, mem_total,
         *            temp, power_draw, power_limit, clk_gr, clk_mem,
         *            pcie_gen, pcie_width, ecc_single, ecc_double */
        char name[64] = {0};
        float power_draw = 0, power_limit = 0;
        int n = sscanf(line,
                       " %u , %63[^,] , %u , %u , %" PRIu64 " , %" PRIu64
                       " , %d , %f , %f , %u , %u"
                       " , %*u , %*u , %" PRIu64 " , %" PRIu64,
                       &g->gpu_index, name, &g->gpu_util_pct, &g->mem_util_pct,
                       &g->mem_used_mib, &g->mem_total_mib,
                       &g->temperature_c, &power_draw, &power_limit,
                       &g->clock_graphics_mhz, &g->clock_mem_mhz,
                       &g->ecc_single_bit, &g->ecc_double_bit);

        if (n >= 7) {
            /* Trim name whitespace */
            char *s = name;
            while (*s == ' ') s++;
            char *e = s + strlen(s) - 1;
            while (e > s && *e == ' ') *e-- = '\0';
            snprintf(g->name, sizeof(g->name), "%s", s);

            g->power_draw_mw  = (uint32_t)(power_draw * 1000.0f);
            g->power_limit_mw = (uint32_t)(power_limit * 1000.0f);
            g->valid = 1;
            out->num_gpus++;
        }
    }
    pclose(fp);

    return 0;
}

/* ===============================================================
 * Container Discovery
 * =============================================================== */
int aurora_infra_discover_container(aurora_infra_config_t *cfg)
{
    /* Strategy 1: If container_name is given, find its PID via Docker */
    if (cfg->container_name[0] != '\0') {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "docker inspect --format '{{.State.Pid}}' '%s' 2>/dev/null",
                 cfg->container_name);
        FILE *fp = popen(cmd, "r");
        if (fp) {
            char buf[64];
            if (fgets(buf, sizeof(buf), fp))
                cfg->target_pid = (pid_t)atoi(buf);
            pclose(fp);
        }

        /* Find cgroup path via docker inspect .Id (reliable across cgroupns) */
        if (cfg->target_pid > 0 && cfg->cgroup_path[0] == '\0') {
            char icmd[256], idbuf[128];
            snprintf(icmd, sizeof(icmd),
                     "docker inspect --format '{{.Id}}' '%s' 2>/dev/null",
                     cfg->container_name);
            FILE *ifp = popen(icmd, "r");
            if (ifp) {
                if (fgets(idbuf, sizeof(idbuf), ifp)) {
                    idbuf[strcspn(idbuf, "\n")] = '\0';
                    snprintf(cfg->cgroup_path, sizeof(cfg->cgroup_path),
                             "%.64s/sys/fs/cgroup/system.slice/docker-%.64s.scope",
                             cfg->host_sys_prefix, idbuf);
                }
                pclose(ifp);
            }
        }
        return (cfg->target_pid > 0) ? 0 : -1;
    }

    /* Strategy 2: Auto-discover — find container running the OCUDU image */
    FILE *fp = popen(
        "docker ps --format '{{.Names}}' --filter 'ancestor=ocudu/gnb' 2>/dev/null",
        "r");
    if (fp) {
        char name[128];
        if (fgets(name, sizeof(name), fp)) {
            name[strcspn(name, "\n")] = '\0';
            snprintf(cfg->container_name, sizeof(cfg->container_name), "%s", name);
            pclose(fp);
            /* Recurse with the discovered name */
            return aurora_infra_discover_container(cfg);
        }
        pclose(fp);
    }

    return -1;
}

/* ===============================================================
 * Auto-select container name by ran_type hint
 *   Queries `docker ps` and picks first container whose image name
 *   matches the OCUDU naming for the ran_type.
 * =============================================================== */
int aurora_infra_auto_select_container(aurora_infra_config_t *cfg,
                                       const char *ran_type_str)
{
    if (!cfg) return -1;
    if (cfg->container_name[0] != '\0') return 0;  /* already set */
    if (!ran_type_str) ran_type_str = "gnb";

    FILE *fp = popen(
        "docker ps --format '{{.Names}}\t{{.Image}}' 2>/dev/null", "r");
    if (!fp) {
        fprintf(stderr, "AURORA_INFRA: docker ps failed (docker CLI + socket required)\n");
        return -1;
    }

    char line[512];
    char best[AURORA_INFRA_CONTAINER_NAME_LEN] = {0};
    while (fgets(line, sizeof(line), fp)) {
        line[strcspn(line, "\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        const char *name  = line;
        const char *image = tab + 1;

        (void)image;  /* image tag (ocudu/gnb for every role) does not identify the role */
        /* Exclude sidecar readers and peer services */
        if (strstr(name, "reader"))    continue;
        if (strstr(name, "nr-ue"))     continue;
        if (strstr(name, "amf"))       continue;
        if (strstr(name, "smf"))       continue;
        if (strstr(name, "upf"))       continue;
        if (strstr(name, "ext-dn"))    continue;
        if (strstr(name, "mysql"))     continue;

        /* OCUDU docker-compose names: ocudu_gnb, ocudu_cu, ocudu_du, ocudu_cu_cp, ocudu_cu_up
         * (docker/docker-compose*.yml). "-"/"_" separators both accepted. */
        int is_cucp = (strstr(name, "cu_cp") != NULL || strstr(name, "cu-cp") != NULL);
        int is_cuup = (strstr(name, "cu_up") != NULL || strstr(name, "cu-up") != NULL);
        int is_cu   = (strstr(name, "ocudu_cu") != NULL || strstr(name, "ocudu-cu") != NULL);
        int is_du   = (strstr(name, "ocudu_du") != NULL || strstr(name, "ocudu-du") != NULL);
        int is_gnb  = (strstr(name, "ocudu_gnb") != NULL || strstr(name, "ocudu-gnb") != NULL);

        int match = 0;
        if (!strcmp(ran_type_str, "cu")) {
            match = is_cu && !is_cuup && !is_cucp;
        } else if (!strcmp(ran_type_str, "du")) {
            match = is_du;
        } else if (!strcmp(ran_type_str, "cu_cp") || !strcmp(ran_type_str, "cu-cp")) {
            match = is_cucp;
        } else if (!strcmp(ran_type_str, "cu_up") || !strcmp(ran_type_str, "cu-up")) {
            match = is_cuup;
        } else {
            match = is_gnb || is_cu || is_du;
        }
        if (match) {
            snprintf(best, sizeof(best), "%.*s",
                     (int)(sizeof(best) - 1), name);
            break;
        }
    }
    pclose(fp);

    if (best[0] == '\0') {
        fprintf(stderr, "AURORA_INFRA: auto-select: no container matched ran_type=%s\n",
                ran_type_str);
        return -1;
    }
    snprintf(cfg->container_name, sizeof(cfg->container_name), "%s", best);
    fprintf(stderr, "AURORA_INFRA: auto-selected container '%s' for ran_type=%s\n",
            cfg->container_name, ran_type_str);
    return 0;
}
