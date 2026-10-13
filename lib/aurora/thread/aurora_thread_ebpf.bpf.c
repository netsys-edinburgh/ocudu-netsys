// SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause
/*
 * aurora_thread_ebpf.bpf.c — eBPF kernel programs for thread runtime collection.
 *
 * Compiled with: clang -target bpf -O2 -g -c aurora_thread_ebpf.bpf.c
 * Requires: Linux kernel >= 5.4 with BTF support, libbpf >= 0.6.
 *
 * BPF programs:
 *   1. handle_sched_switch  (tp/sched/sched_switch)
 *      - Records per-quantum runtime for monitored threads
 *      - Fills exponential histogram in tid_stats map
 *
 *   2. handle_irq_entry     (tp/irq/irq_handler_entry)
 *      - Counts IRQ interruptions during thread execution
 *
 * BPF maps:
 *   tid_run_start  — per-RAW-TID on-CPU start timestamp (ns); keyed by the
 *                    host pid-namespace TID straight off the tracepoint,
 *                    since it is pure internal scratch state never compared
 *                    against userspace's namespace-relative TIDs
 *   tid_stats      — per-NAMESPACE-RELATIVE-TID accumulated statistics
 *                    (aurora_bpf_thread_stats)
 *   tid_filter     — namespace-relative TID → 1 (only monitored TIDs,
 *                    updated from user space)
 *   raw_to_ns_tid  — raw host TID → namespace-relative TID, populated
 *                    opportunistically in handle_sched_switch; lets
 *                    handle_sched_migrate resolve a TID it can't translate
 *                    directly (see comment there)
 *
 * PID-namespace translation:
 *   ctx->prev_pid / ctx->next_pid (sched_switch) and ctx->pid
 *   (sched_migrate_task) are always host pid-namespace TIDs — the raw
 *   kernel task_struct::pid — regardless of which namespace this program
 *   was loaded from. Userspace (aurora_thread_ebpf.c) populates tid_filter
 *   using TIDs from its OWN /proc/self/task walk, which are relative to
 *   its own (e.g. container) pid namespace. Comparing the two directly
 *   silently attributes an unrelated task's scheduling data to whatever
 *   monitored thread happens to share its numeric TID in a different
 *   namespace (observed in practice: host per-CPU kernel threads
 *   migration/N and ksoftirqd/N colliding with low in-container TIDs).
 *   This program uses bpf_get_ns_current_pid_tgid(), keyed against the
 *   target namespace's dev/ino (aurora_target_ns_dev/ino, set by userspace
 *   into this object's .rodata before load), to translate — and, as a
 *   side effect, authenticate — "current" into that namespace. It returns
 *   nonzero if "current" does not belong to the target namespace at all,
 *   which is exactly the filter needed to drop unrelated host/other-
 *   container tasks. It only ever translates "current", so it is only
 *   directly usable where "current" is known to be the task of interest —
 *   see the per-hook comments below for where that is and isn't true.
 */

#include "vmlinux.h"          /* auto-generated from BTF: bpftool btf dump file /sys/kernel/btf/vmlinux format c */
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>
#include "aurora_thread_ebpf.h"

#define MAX_TIDS 1024
#define MAX_RAW_TIDS 4096  /* host-wide TID churn is much higher than MAX_TIDS's per-namespace budget */

/* Target pid namespace identity (dev/ino of its /proc/self/ns/pid), set by
 * userspace into .rodata after bpf_object__open() and before load — see
 * aurora_ebpf_start() in aurora_thread_ebpf.c. const volatile so the
 * verifier treats it as read-only but not constant-folded away. */
const volatile __u64 aurora_target_ns_dev = 0;
const volatile __u64 aurora_target_ns_ino = 0;

/* Kernel's __trace_sched_switch_state() returns this verbatim, ignoring task
 * state, for in-kernel preemption ("R+"); the bits below it carry the
 * reported task state (0 = still runnable, "R").
 * Value per include/linux/sched.h: TASK_REPORT is the OR of TASK_RUNNING(0x0)
 * | TASK_INTERRUPTIBLE(0x1) | TASK_UNINTERRUPTIBLE(0x2) | __TASK_STOPPED(0x4)
 * | __TASK_TRACED(0x8) | EXIT_DEAD(0x10) | EXIT_ZOMBIE(0x20) | TASK_PARKED(0x40)
 * = 0x7F; TASK_REPORT_IDLE = TASK_REPORT + 1 = 0x80; TASK_REPORT_MAX =
 * TASK_REPORT_IDLE << 1 = 0x100. Do NOT confuse with TASK_STATE_MAX (0x10000,
 * an unrelated mask width constant used only by TASK_ANY). */
#define AURORA_TASK_REPORT_MAX 0x100

/* ---------------------------------------------------------------
 * BPF Maps
 * --------------------------------------------------------------- */

/* RAW host-namespace tid → on-CPU start timestamp (ns, CLOCK_MONOTONIC).
 * Populated unconditionally for every switch-in on the host (not just
 * monitored threads — "next" can't be namespace-translated at switch time,
 * see handle_sched_switch), so this needs a bigger, LRU-bounded budget
 * rather than MAX_TIDS's per-namespace-monitored-thread sizing. */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_RAW_TIDS);
    __type(key, __u32);
    __type(value, __u64);
} tid_run_start SEC(".maps");

/* NAMESPACE-RELATIVE tid → accumulated stats */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TIDS);
    __type(key, __u32);
    __type(value, struct aurora_bpf_thread_stats);
} tid_stats SEC(".maps");

/* Current collection interval number (single element). User space writes the
 * next number at the end of every collection interval; it is the only map
 * entry user space writes while the programs run, apart from tid_filter. */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} interval_epoch SEC(".maps");

/* NAMESPACE-RELATIVE tid → 1u8: filter for monitored TIDs (populated from
 * user space, using its own /proc/self/task walk). */
struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, MAX_TIDS);
    __type(key, __u32);
    __type(value, __u8);
} tid_filter SEC(".maps");

/* RAW host-namespace tid → namespace-relative tid, populated opportunistically
 * in handle_sched_switch whenever a thread is seen going off-CPU with a
 * successful namespace translation. Lets handle_sched_migrate resolve a TID
 * it cannot translate directly (its ctx->pid is not reliably "current"). */
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, MAX_TIDS);
    __type(key, __u32);
    __type(value, __u32);
} raw_to_ns_tid SEC(".maps");

/* ---------------------------------------------------------------
 * Helper: compute exponential histogram bin
 * Returns i such that runtime_ns ∈ [2^i, 2^(i+1) - 1].
 * Returns AURORA_THREAD_HIST_BINS for overflow.
 * --------------------------------------------------------------- */
static __always_inline int aurora_bpf_log2_bin(__u64 v)
{
    if (v == 0) return 0;
    int bin = 0;
    /* Unrolled log2 safe for BPF verifier (no loops) */
    if (v >= (1ULL << 32)) return AURORA_THREAD_HIST_BINS;
    if (v >= (1ULL << 16)) { bin += 16; v >>= 16; }
    if (v >= (1ULL <<  8)) { bin +=  8; v >>=  8; }
    if (v >= (1ULL <<  4)) { bin +=  4; v >>=  4; }
    if (v >= (1ULL <<  2)) { bin +=  2; v >>=  2; }
    if (v >= (1ULL <<  1)) { bin +=  1; }
    return (bin < AURORA_THREAD_HIST_BINS) ? bin : AURORA_THREAD_HIST_BINS;
}

/* ---------------------------------------------------------------
 * Helper: look up a TID's stats entry, inserting a sentinel-initialized
 * zeroed entry on first sight.
 * --------------------------------------------------------------- */
static __always_inline struct aurora_bpf_thread_stats *aurora_bpf_get_or_init(__u32 tid, __u32 pid)
{
    struct aurora_bpf_thread_stats *s = bpf_map_lookup_elem(&tid_stats, &tid);
    if (s)
        return s;

    struct aurora_bpf_thread_stats zero = {};
    zero.pid = pid;
    zero.last_cpu = AURORA_BPF_CPU_UNSET;
    bpf_map_update_elem(&tid_stats, &tid, &zero, BPF_NOEXIST);
    return bpf_map_lookup_elem(&tid_stats, &tid);
}

/* ---------------------------------------------------------------
 * Helper: record quantum runtime for a TID into tid_stats map
 * --------------------------------------------------------------- */
static __always_inline void aurora_bpf_record_quantum(
    __u32 tid, __u32 pid, __u64 runtime_ns, __u32 cpu)
{
    struct aurora_bpf_thread_stats *s = aurora_bpf_get_or_init(tid, pid);
    if (!s)
        return;

    int bin = aurora_bpf_log2_bin(runtime_ns);
    if (bin < AURORA_THREAD_HIST_BINS)
        __sync_fetch_and_add(&s->hist_runtime[bin], 1);
    else
        __sync_fetch_and_add(&s->overflow, 1);

    __sync_fetch_and_add(&s->total_events, 1);

    /* Atomic max update: CAS loop (BPF verifier allows this pattern) */
    __u64 old_max = s->max_runtime_ns;
    if (runtime_ns > old_max)
        __sync_val_compare_and_swap(&s->max_runtime_ns, old_max, runtime_ns);

    /* Per-interval maximum. Quanta of a TID are only recorded by the
     * sched_switch that takes that thread off its CPU, so updates of one
     * entry never run concurrently and need no atomics. A stale epoch means
     * this is the thread's first quantum of the current interval. */
    __u32 epoch_key = 0;
    __u64 *epoch = bpf_map_lookup_elem(&interval_epoch, &epoch_key);
    __u64 cur_epoch = epoch ? *epoch : 0;
    if (s->interval_epoch != cur_epoch) {
        s->interval_max_runtime_ns = runtime_ns;
        s->interval_epoch          = cur_epoch;
    } else if (runtime_ns > s->interval_max_runtime_ns) {
        s->interval_max_runtime_ns = runtime_ns;
    }

    s->last_cpu = cpu;
    s->pid      = pid;
}

/* ---------------------------------------------------------------
 * Tracepoint: sched/sched_switch
 * Fires on every context switch.
 * --------------------------------------------------------------- */
SEC("tp/sched/sched_switch")
int handle_sched_switch(struct trace_event_raw_sched_switch *ctx)
{
    __u64 ts = bpf_ktime_get_ns();
    __u32 prev_raw_tid = (__u32)ctx->prev_pid;
    __u32 next_raw_tid = (__u32)ctx->next_pid;

    /* This tracepoint fires from __schedule() before switch_to() runs, so
     * "current" here is still the OUTGOING (prev) task — bpf_get_current_*
     * and bpf_get_ns_current_pid_tgid() both refer to prev, never next.
     * That makes prev (and only prev) translatable/authenticatable via the
     * namespace helper at this hook; see the file header comment. A
     * nonzero return means prev is not in our target namespace at all
     * (e.g. an unrelated host kernel thread) and must be ignored — this is
     * the fix for cross-namespace TID collisions. */
    struct bpf_pidns_info ns = {};
    if (bpf_get_ns_current_pid_tgid(aurora_target_ns_dev, aurora_target_ns_ino,
                                     &ns, sizeof(ns)) == 0) {
        __u32 prev_ns_tid = ns.pid;

        __u8 *monitored = bpf_map_lookup_elem(&tid_filter, &prev_ns_tid);
        if (monitored) {
            /* tid_run_start is keyed by the RAW tid — it was stashed by this
             * same thread's own earlier switch-IN below, before it had a
             * translated identity available (see the "next" handling). */
            __u64 *start = bpf_map_lookup_elem(&tid_run_start, &prev_raw_tid);
            if (start && *start > 0 && ts >= *start) {
                __u64 delta = ts - *start;
                __u32 cpu = bpf_get_smp_processor_id();
                aurora_bpf_record_quantum(prev_ns_tid, ns.tgid, delta, cpu);
            }

            /* OCUDU fix: classify like the kernel's own nvcsw/nivcsw accounting in __schedule(): the switch is
             * involuntary when the outgoing task is still runnable. That is prev_state == 0 ("R"), or the
             * TASK_REPORT_MAX bit 0x100 ("R+") for in-kernel preemption. User-space threads are preempted through
             * schedule() on return to user mode, which reports plain "R" (verified on 6.8 with tracefs), so the OAI
             * rule "involuntary only if prev_state == 0x100" counted every user-space preemption as voluntary.
             * Any blocked/stopped state bit (S, D, T, t, P, I, ...) means the thread gave up the CPU itself. */
            struct aurora_bpf_thread_stats *ps = aurora_bpf_get_or_init(prev_ns_tid, ns.tgid);
            if (ps) {
                if ((ctx->prev_state & (AURORA_TASK_REPORT_MAX - 1)) == 0)
                    __sync_fetch_and_add(&ps->involuntary_ctxsw, 1);
                else
                    __sync_fetch_and_add(&ps->voluntary_ctxsw, 1);
            }

            /* Let handle_sched_migrate (whose ctx->pid isn't reliably
             * "current") resolve this raw tid back to its ns-relative one. */
            bpf_map_update_elem(&raw_to_ns_tid, &prev_raw_tid, &prev_ns_tid, BPF_ANY);
        }

        /* Clear this thread's start timestamp now that its quantum (if any)
         * has been recorded above. */
        __u64 zero_ts = 0;
        bpf_map_update_elem(&tid_run_start, &prev_raw_tid, &zero_ts, BPF_ANY);
    }

    /* next thread going on-CPU: "current" hasn't switched to it yet, so it
     * cannot be namespace-translated/authenticated here (see file header
     * comment) — stash its wakeup timestamp keyed by its RAW tid
     * unconditionally instead (tid_run_start is LRU-bounded across
     * MAX_RAW_TIDS precisely to make this host-wide, filter-less stashing
     * cheap). The namespace translation and whitelist check happen above,
     * deferred until this same thread is later seen going off-CPU as prev. */
    bpf_map_update_elem(&tid_run_start, &next_raw_tid, &ts, BPF_ANY);

    return 0;
}

/* ---------------------------------------------------------------
 * Tracepoint: sched/sched_migrate_task
 * Fires when the scheduler moves a task to a different CPU.
 * --------------------------------------------------------------- */
SEC("tp/sched/sched_migrate_task")
int handle_sched_migrate(struct trace_event_raw_sched_migrate_task *ctx)
{
    __u32 raw_tid = (__u32)ctx->pid;

    /* ctx->pid is the task BEING migrated, which is not reliably "current"
     * here (e.g. a load-balancer running on a different CPU can migrate a
     * task it isn't itself) — so bpf_get_ns_current_pid_tgid() doesn't
     * apply directly the way it does in handle_sched_switch/
     * handle_irq_entry. Resolve via the raw->ns mapping opportunistically
     * recorded in handle_sched_switch instead. If this raw tid was never
     * observed going off-CPU yet, the migration is simply not counted —
     * rare, and harmless (no data gets misattributed to the wrong thread,
     * unlike the untranslated-comparison bug this file fixes). */
    __u32 *ns_tid = bpf_map_lookup_elem(&raw_to_ns_tid, &raw_tid);
    if (!ns_tid)
        return 0;

    __u8 *monitored = bpf_map_lookup_elem(&tid_filter, ns_tid);
    if (!monitored)
        return 0;

    struct aurora_bpf_thread_stats *s = aurora_bpf_get_or_init(*ns_tid, 0);
    if (s)
        __sync_fetch_and_add(&s->cpu_migrations, 1);

    return 0;
}

/* ---------------------------------------------------------------
 * Interrupt accounting
 *
 * OCUDU: irq_count counts every interrupt that preempts a monitored thread:
 * device IRQs (irq/irq_handler_entry), the x86 system interrupt vectors
 * (irq_vectors/<vector>_entry: local APIC timer, rescheduling and
 * function-call IPIs, IRQ work, ...) and softirqs run in its context
 * (irq/softirq_entry). OAI only hooked irq_handler_entry, which never fires
 * for the local timer, so the ~1k-per-CPU-per-second timer interrupts were
 * missing. The irq_vectors tracepoints only exist on x86; user space
 * attaches them when available (aurora_thread_ebpf.c).
 *
 * In every one of these hooks "current" is the interrupted task, so it
 * translates directly via the namespace helper.
 * --------------------------------------------------------------- */
static __always_inline int aurora_bpf_count_interrupt(void)
{
    struct bpf_pidns_info ns = {};
    if (bpf_get_ns_current_pid_tgid(aurora_target_ns_dev, aurora_target_ns_ino,
                                     &ns, sizeof(ns)) != 0)
        return 0; /* interrupted task is not in our target namespace */

    __u8 *monitored = bpf_map_lookup_elem(&tid_filter, &ns.pid);
    if (!monitored)
        return 0;

    struct aurora_bpf_thread_stats *s = bpf_map_lookup_elem(&tid_stats, &ns.pid);
    if (s)
        __sync_fetch_and_add(&s->irq_count, 1);

    return 0;
}

SEC("tp/irq/irq_handler_entry")
int handle_irq_entry(void *ctx)
{
    return aurora_bpf_count_interrupt();
}

SEC("tp/irq/softirq_entry")
int handle_softirq_entry(void *ctx)
{
    return aurora_bpf_count_interrupt();
}

/* x86 system interrupt vectors (optional, see above). */
#define AURORA_IRQ_VECTOR(vec)                                  \
    SEC("tp/irq_vectors/" #vec "_entry")                        \
    int handle_##vec##_entry(void *ctx)                         \
    {                                                           \
        return aurora_bpf_count_interrupt();                    \
    }

AURORA_IRQ_VECTOR(local_timer)
AURORA_IRQ_VECTOR(reschedule)
AURORA_IRQ_VECTOR(call_function)
AURORA_IRQ_VECTOR(call_function_single)
AURORA_IRQ_VECTOR(irq_work)
AURORA_IRQ_VECTOR(x86_platform_ipi)
AURORA_IRQ_VECTOR(thermal_apic)
AURORA_IRQ_VECTOR(threshold_apic)
AURORA_IRQ_VECTOR(deferred_error_apic)
AURORA_IRQ_VECTOR(error_apic)
AURORA_IRQ_VECTOR(spurious_apic)

char LICENSE[] SEC("license") = "Dual BSD/GPL";
