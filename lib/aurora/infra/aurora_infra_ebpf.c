/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * aurora_infra_ebpf.c — Userspace libbpf loader for infra eBPF program.
 *
 * Responsibilities:
 *   1. Load + attach aurora_infra_ebpf skeleton.
 *   2. Compute cgroup id from cfg->cgroup_path (stat().st_ino).
 *   3. Push target cgroup id into target_cgid array map.
 *   4. Expose collectors that convert monotonic kernel counters into
 *      per-interval deltas merged into aurora_container_*_t.
 */

#include "aurora_infra_ebpf.h"
#include "aurora_infra.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <pthread.h>
#include <net/if.h>

#ifdef AURORA_INFRA_EBPF_ENABLED

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "aurora_infra_ebpf.skel.h"

static struct aurora_infra_ebpf_bpf *g_skel = NULL;
static __u64                        g_target_cgid = 0;
static pthread_mutex_t              g_lock = PTHREAD_MUTEX_INITIALIZER;
static int                          g_active = 0;

/* Previous counters for delta computation. */
static struct aurora_infra_cg_stats g_prev = {0};
static int                          g_have_prev = 0;

static int libbpf_silent(enum libbpf_print_level lvl, const char *fmt, va_list args)
{
    if (lvl == LIBBPF_WARN) return vfprintf(stderr, fmt, args);
    return 0;
}

static int bump_memlock(void)
{
    /* libbpf >= 1.0 uses memcg-based BPF memory accounting; setrlimit is
     * best-effort. Ignore EPERM from rootless/cap-restricted contexts. */
    struct rlimit r = { RLIM_INFINITY, RLIM_INFINITY };
    (void)setrlimit(RLIMIT_MEMLOCK, &r);
    return 0;
}

static __u64 compute_cgroup_id(const char *cgroup_path)
{
    if (!cgroup_path || !*cgroup_path) return 0;
    struct stat st;
    if (stat(cgroup_path, &st) != 0) {
        fprintf(stderr, "AURORA_INFRA_EBPF: stat('%s') failed: %s\n",
                cgroup_path, strerror(errno));
        return 0;
    }
    return (__u64)st.st_ino;
}

int aurora_infra_ebpf_init(const aurora_infra_config_t *cfg)
{
    if (!cfg) return -1;
    pthread_mutex_lock(&g_lock);
    if (g_skel) { pthread_mutex_unlock(&g_lock); return 0; }

    libbpf_set_print(libbpf_silent);
    bump_memlock();

    g_skel = aurora_infra_ebpf_bpf__open_and_load();
    if (!g_skel) {
        fprintf(stderr, "AURORA_INFRA_EBPF: skeleton open/load failed: %s\n",
                strerror(errno));
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    if (aurora_infra_ebpf_bpf__attach(g_skel) != 0) {
        fprintf(stderr, "AURORA_INFRA_EBPF: attach failed: %s\n",
                strerror(errno));
        aurora_infra_ebpf_bpf__destroy(g_skel);
        g_skel = NULL;
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    /* Compute + push target cgroup id. */
    g_target_cgid = compute_cgroup_id(cfg->cgroup_path);
    __u32 k = 0;
    int target_cgid_fd = bpf_map__fd(g_skel->maps.target_cgid);
    if (target_cgid_fd < 0 ||
        bpf_map_update_elem(target_cgid_fd, &k, &g_target_cgid, BPF_ANY) != 0) {
        fprintf(stderr, "AURORA_INFRA_EBPF: failed to set target_cgid=%llu\n",
                (unsigned long long)g_target_cgid);
    }

    g_active = 1;
    fprintf(stderr, "AURORA_INFRA_EBPF: initialized — cgroup='%s' cgid=%llu\n",
            cfg->cgroup_path, (unsigned long long)g_target_cgid);
    pthread_mutex_unlock(&g_lock);
    return 0;
}

void aurora_infra_ebpf_shutdown(void)
{
    pthread_mutex_lock(&g_lock);
    if (g_skel) {
        aurora_infra_ebpf_bpf__destroy(g_skel);
        g_skel = NULL;
    }
    g_active = 0;
    pthread_mutex_unlock(&g_lock);
}

int aurora_infra_ebpf_is_active(void)
{
    return g_active;
}

/* Read monotonic counters for the target cgroup. */
static int ebpf_read_stats(struct aurora_infra_cg_stats *out)
{
    if (!g_skel || g_target_cgid == 0) return -1;
    int fd = bpf_map__fd(g_skel->maps.cg_stats);
    if (fd < 0) return -1;
    if (bpf_map_lookup_elem(fd, &g_target_cgid, out) != 0) {
        /* No events yet — return zeros, not an error. */
        memset(out, 0, sizeof(*out));
    }
    return 0;
}

/* Compute deltas and update previous snapshot. */
static void ebpf_snapshot_delta(struct aurora_infra_cg_stats *delta)
{
    struct aurora_infra_cg_stats cur = {0};
    if (ebpf_read_stats(&cur) != 0) { memset(delta, 0, sizeof(*delta)); return; }
    if (!g_have_prev) {
        memset(delta, 0, sizeof(*delta));
        g_prev = cur;
        g_have_prev = 1;
        return;
    }
    delta->vol_ctxsw    = cur.vol_ctxsw    - g_prev.vol_ctxsw;
    delta->invol_ctxsw  = cur.invol_ctxsw  - g_prev.invol_ctxsw;
    delta->pgfault_user = cur.pgfault_user - g_prev.pgfault_user;
    delta->tcp_retrans  = cur.tcp_retrans  - g_prev.tcp_retrans;
    delta->block_ios    = cur.block_ios    - g_prev.block_ios;
    delta->runtime_ns   = cur.runtime_ns   - g_prev.runtime_ns;
    g_prev = cur;
}

int aurora_infra_ebpf_collect_container_cpu(aurora_container_cpu_t *out)
{
    if (!g_active || !out) return -1;
    struct aurora_infra_cg_stats d;
    pthread_mutex_lock(&g_lock);
    ebpf_snapshot_delta(&d);
    pthread_mutex_unlock(&g_lock);
    out->voluntary_ctxsw   = d.vol_ctxsw;
    out->involuntary_ctxsw = d.invol_ctxsw;
    /* Note: cpu_usage_us/pct left to procfs (cgroup cpu.stat is cheaper
     * and authoritative than accumulating sched_switch deltas). */
    return 0;
}

int aurora_infra_ebpf_collect_container_mem(aurora_container_mem_t *out)
{
    if (!g_active || !out) return -1;
    int fd = bpf_map__fd(g_skel->maps.cg_stats);
    if (fd < 0) return -1;
    struct aurora_infra_cg_stats cur = {0};
    if (bpf_map_lookup_elem(fd, &g_target_cgid, &cur) != 0) return 0;
    static __u64 prev_pgfault = 0;
    static int   have_prev_pg = 0;
    __u64 delta = have_prev_pg ? (cur.pgfault_user - prev_pgfault) : 0;
    prev_pgfault = cur.pgfault_user;
    have_prev_pg = 1;
    out->mem_pgfault = delta;
    return 0;
}

int aurora_infra_ebpf_collect_container_net(aurora_container_net_t *out)
{
    if (!g_active || !out) return -1;
    int fd = bpf_map__fd(g_skel->maps.cg_stats);
    if (fd < 0) return -1;
    struct aurora_infra_cg_stats cur = {0};
    if (bpf_map_lookup_elem(fd, &g_target_cgid, &cur) != 0) return 0;
    static __u64 prev_retrans = 0;
    static int   have_prev_rt = 0;
    __u64 delta = have_prev_rt ? (cur.tcp_retrans - prev_retrans) : 0;
    prev_retrans = cur.tcp_retrans;
    have_prev_rt = 1;
    out->tcp_retransmits = delta;
    return 0;
}

int aurora_infra_ebpf_collect_container_io(aurora_container_io_t *out)
{
    if (!g_active || !out) return -1;
    int fd = bpf_map__fd(g_skel->maps.cg_stats);
    if (fd < 0) return -1;
    struct aurora_infra_cg_stats cur = {0};
    if (bpf_map_lookup_elem(fd, &g_target_cgid, &cur) != 0) return 0;
    static __u64 prev_ios = 0;
    static int   have_prev_io = 0;
    __u64 delta = have_prev_io ? (cur.block_ios - prev_ios) : 0;
    prev_ios = cur.block_ios;
    have_prev_io = 1;
    /* cur.block_ios counts block_rq_issue events (block-layer request
     * issues); io_read_ops is a syscall-level read-op count from procfs.
     * These are different units on different layers — do not add them
     * together (that previously silently inflated io_read_ops with an
     * unrelated count). No dedicated block-issue field exists yet, so
     * this eBPF signal is intentionally left uncontributed for now. */
    (void)delta;
    return 0;
}

/*
 * Host-wide NIC carrier transition lookup — not cgroup-scoped, so no
 * delta/prev-snapshot bookkeeping here: the BPF map already holds a
 * monotonic since-attach count per ifindex, read straight through.
 */
int aurora_infra_ebpf_collect_host_nic_carrier(const char *iface_name, uint64_t *out)
{
    if (!g_active || !g_skel || !iface_name || !out) return -1;

    unsigned int ifindex = if_nametoindex(iface_name);
    if (ifindex == 0) return -1; /* interface not found */

    int fd = bpf_map__fd(g_skel->maps.nic_carrier_changes);
    if (fd < 0) return -1;

    __u32 key = (__u32)ifindex;
    __u64 val = 0;
    if (bpf_map_lookup_elem(fd, &key, &val) != 0)
        val = 0; /* no flap observed yet since attach — not an error */

    *out = (uint64_t)val;
    return 0;
}

#else /* !AURORA_INFRA_EBPF_ENABLED — stubs */

int aurora_infra_ebpf_init(const aurora_infra_config_t *cfg) { (void)cfg; return -1; }
void aurora_infra_ebpf_shutdown(void) {}
int aurora_infra_ebpf_is_active(void) { return 0; }
int aurora_infra_ebpf_collect_container_cpu(aurora_container_cpu_t *o) { (void)o; return -1; }
int aurora_infra_ebpf_collect_container_mem(aurora_container_mem_t *o) { (void)o; return -1; }
int aurora_infra_ebpf_collect_container_net(aurora_container_net_t *o) { (void)o; return -1; }
int aurora_infra_ebpf_collect_container_io (aurora_container_io_t  *o) { (void)o; return -1; }
int aurora_infra_ebpf_collect_host_nic_carrier(const char *iface_name, uint64_t *out) {
    (void)iface_name; (void)out; return -1;
}

#endif
