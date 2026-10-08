// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * aurora_infra_ebpf.bpf.c — Infrastructure eBPF collector.
 *
 * Scopes events to a target cgroup id (cgroup v2 inode) pushed from
 * userspace via the target_cgid singleton array map.
 *
 * Tracepoints:
 *   tp/sched/sched_switch            — vol/invol ctxsw + on-CPU runtime
 *   tp/exceptions/page_fault_user    — userspace page faults
 *   tp/tcp/tcp_retransmit_skb        — tcp retransmissions
 *   tp/block/block_rq_issue          — block I/O submissions
 *
 * Kprobes (host-wide, NOT cgroup-filtered — see nic_carrier_changes below):
 *   kprobe/netif_carrier_on          — link up transition, per NIC
 *   kprobe/netif_carrier_off         — link down transition, per NIC
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "aurora_infra_ebpf.h"

#define MAX_CGROUPS 64

/* Singleton map: index 0 holds the target cgroup id. 0 == match-any. */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} target_cgid SEC(".maps");

/* Per-cgroup stats keyed by cgroup id. */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_CGROUPS);
    __type(key, __u64);
    __type(value, struct aurora_infra_cg_stats);
} cg_stats SEC(".maps");

/* Per-pid on-CPU start timestamp (ns), keyed by kernel pid (tid). */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 4096);
    __type(key, __u32);
    __type(value, __u64);
} pid_oncpu_ts SEC(".maps");

/* Host-wide link up/down transition count, keyed by net_device ifindex.
 * NOT cgroup-filtered: netif_carrier_on/off fire in driver/link-watch
 * context, not the monitored task's context, so there is no cgroup to
 * match against here — every NIC on the host is tracked. */
#define MAX_TRACKED_NICS 16
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TRACKED_NICS);
    __type(key, __u32);
    __type(value, __u64);
} nic_carrier_changes SEC(".maps");

/* ---------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------- */
static __always_inline int cg_matches_target(__u64 cg)
{
    __u32 k = 0;
    __u64 *tgt = bpf_map_lookup_elem(&target_cgid, &k);
    if (!tgt) return 0;
    if (*tgt == 0) return 1;        /* 0 == match-any */
    return (cg == *tgt);
}

static __always_inline struct aurora_infra_cg_stats *
cg_stats_get_or_init(__u64 cg)
{
    struct aurora_infra_cg_stats *s = bpf_map_lookup_elem(&cg_stats, &cg);
    if (s) return s;
    struct aurora_infra_cg_stats zero = {};
    bpf_map_update_elem(&cg_stats, &cg, &zero, BPF_NOEXIST);
    return bpf_map_lookup_elem(&cg_stats, &cg);
}

/* ---------------------------------------------------------------
 * sched_switch — record ctxsw + runtime
 * prev_state != TASK_RUNNING  → voluntary ctxsw
 * prev_state == TASK_RUNNING  → involuntary (preempted)
 * --------------------------------------------------------------- */
SEC("tp/sched/sched_switch")
int aurora_infra_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    __u64 ts  = bpf_ktime_get_ns();
    __u32 prev_tid = (__u32)ctx->prev_pid;
    __u32 next_tid = (__u32)ctx->next_pid;
    long  prev_state = ctx->prev_state;

    /* Grab cgroup id at sched_switch context — attributed to the CURRENT
     * task, which during the sched_switch tracepoint is the prev task. */
    __u64 cg = bpf_get_current_cgroup_id();
    if (cg_matches_target(cg)) {
        struct aurora_infra_cg_stats *s = cg_stats_get_or_init(cg);
        if (s) {
            if (prev_state == 0 /* TASK_RUNNING */)
                __sync_fetch_and_add(&s->invol_ctxsw, 1);
            else
                __sync_fetch_and_add(&s->vol_ctxsw, 1);

            /* Close on-CPU window for prev_tid */
            __u64 *start = bpf_map_lookup_elem(&pid_oncpu_ts, &prev_tid);
            if (start && *start > 0 && ts >= *start) {
                __u64 delta = ts - *start;
                __sync_fetch_and_add(&s->runtime_ns, delta);
            }
            __u64 zero = 0;
            bpf_map_update_elem(&pid_oncpu_ts, &prev_tid, &zero, BPF_ANY);
        }
    }

    /* Track on-CPU start for next task unconditionally; cheap and matched
     * at the close window. */
    bpf_map_update_elem(&pid_oncpu_ts, &next_tid, &ts, BPF_ANY);
    return 0;
}

/* ---------------------------------------------------------------
 * page_fault_user — userspace page faults
 * --------------------------------------------------------------- */
SEC("tp/exceptions/page_fault_user")
int aurora_infra_page_fault_user(void *ctx)
{
    __u64 cg = bpf_get_current_cgroup_id();
    if (!cg_matches_target(cg)) return 0;
    struct aurora_infra_cg_stats *s = cg_stats_get_or_init(cg);
    if (s) __sync_fetch_and_add(&s->pgfault_user, 1);
    return 0;
}

/* ---------------------------------------------------------------
 * tcp_retransmit_skb — TCP retransmissions
 * --------------------------------------------------------------- */
SEC("tp/tcp/tcp_retransmit_skb")
int aurora_infra_tcp_retrans(void *ctx)
{
    __u64 cg = bpf_get_current_cgroup_id();
    if (!cg_matches_target(cg)) return 0;
    struct aurora_infra_cg_stats *s = cg_stats_get_or_init(cg);
    if (s) __sync_fetch_and_add(&s->tcp_retrans, 1);
    return 0;
}

/* ---------------------------------------------------------------
 * block_rq_issue — block I/O submitted
 * --------------------------------------------------------------- */
SEC("tp/block/block_rq_issue")
int aurora_infra_block_rq_issue(void *ctx)
{
    __u64 cg = bpf_get_current_cgroup_id();
    if (!cg_matches_target(cg)) return 0;
    struct aurora_infra_cg_stats *s = cg_stats_get_or_init(cg);
    if (s) __sync_fetch_and_add(&s->block_ios, 1);
    return 0;
}

/* ---------------------------------------------------------------
 * netif_carrier_on/off — link up/down transitions, per NIC.
 * No stable tracepoint exists for carrier state changes, so these are
 * kprobes on the (exported, stable) kernel functions directly. Both take
 * `struct net_device *dev` as their sole argument.
 * --------------------------------------------------------------- */
static __always_inline void bump_carrier_count(struct net_device *dev)
{
    __u32 ifindex = BPF_CORE_READ(dev, ifindex);
    __u64 *cnt = bpf_map_lookup_elem(&nic_carrier_changes, &ifindex);
    if (cnt) {
        __sync_fetch_and_add(cnt, 1);
        return;
    }
    __u64 one = 1;
    bpf_map_update_elem(&nic_carrier_changes, &ifindex, &one, BPF_NOEXIST);
}

SEC("kprobe/netif_carrier_on")
int BPF_KPROBE(aurora_infra_carrier_on, struct net_device *dev)
{
    bump_carrier_count(dev);
    return 0;
}

SEC("kprobe/netif_carrier_off")
int BPF_KPROBE(aurora_infra_carrier_off, struct net_device *dev)
{
    bump_carrier_count(dev);
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
