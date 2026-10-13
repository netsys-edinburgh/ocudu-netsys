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

#ifndef AURORA_THREAD_EBPF_H
#define AURORA_THREAD_EBPF_H

/*
 * aurora_thread_ebpf.h — types shared between the BPF kernel program
 * (aurora_thread_ebpf.bpf.c) and the user-space libbpf agent
 * (aurora_thread_ebpf.c).
 *
 * This header must be compatible with both BPF C and regular C.
 * Use only basic integer types — no pointer types, no complex structs.
 */

#ifndef AURORA_THREAD_HIST_BINS
#define AURORA_THREAD_HIST_BINS  32
#endif

/* __u32/__u64 come from vmlinux.h in BPF context, linux/types.h otherwise. */
#if !defined(__VMLINUX_H__) && !defined(__BPF__)
#include <linux/types.h>
#endif

/*
 * Per-TID statistics accumulated in the BPF hash map.
 *
 * Updated atomically from the sched_switch and irq_handler tracepoints.
 * Read by the user-space agent once per interval and merged into shared memory.
 *
 * OCUDU: the counters are cumulative for the life of the thread and user space
 * never writes to an entry. It looks up each monitored TID and computes the
 * per-interval deltas against the totals it read the previous interval. (The
 * OAI agent reset every entry while walking the map with get_next_key; a hash
 * map update re-inserts the element at the head of its bucket, so two TIDs
 * sharing a bucket made the walk loop forever.)
 */
struct aurora_bpf_thread_stats {
    __u64 hist_runtime[AURORA_THREAD_HIST_BINS]; /* Exponential histogram bins */
    __u64 overflow;       /* Quanta with runtime > 2^31 ns */
    __u64 total_events;   /* Total scheduling quanta recorded */
    __u64 max_runtime_ns; /* Maximum quantum runtime observed since the thread was first seen */
    __u64 irq_count;      /* IRQs/softirqs that interrupted this thread */
    __u64 voluntary_ctxsw;   /* Off-CPU switches where prev_state != TASK_RUNNING */
    __u64 involuntary_ctxsw; /* Off-CPU switches while prev_state == TASK_RUNNING (preemption) */
    __u64 cpu_migrations;    /* sched_migrate_task events for this TID */
    __u64 interval_max_runtime_ns; /* Maximum quantum runtime observed during interval_epoch */
    __u64 interval_epoch;    /* Collection interval interval_max_runtime_ns belongs to */
    __u32 last_cpu;       /* Last CPU the thread ran on; AURORA_BPF_CPU_UNSET if never observed */
    __u32 pid;            /* Process ID (tgid in kernel terms) */
};

/* Sentinel for aurora_bpf_thread_stats.last_cpu meaning "no observation yet",
 * so a thread that never ran isn't reported as running on CPU 0. */
#define AURORA_BPF_CPU_UNSET 0xFFFFFFFFu

/*
 * Key used in the tid_filter BPF map.
 * Value is always 1 (u8) to indicate "monitor this TID".
 *
 * All TIDs in tid_filter, tid_stats and the aurora_bpf_thread_stats.pid
 * field are relative to the userspace agent's OWN pid namespace (the
 * container's, in a containerized deployment) — the same namespace
 * aurora_thread_registry.c's /proc/self/task walk uses. The BPF kernel
 * program translates raw (host pid-namespace) tracepoint TIDs into this
 * namespace via bpf_get_ns_current_pid_tgid() before touching either map;
 * see aurora_thread_ebpf.bpf.c. A raw host TID must never be compared
 * against these maps directly — different pid namespaces routinely reuse
 * the same small integers for unrelated tasks (e.g. host per-CPU kernel
 * threads like migration/N), so an untranslated comparison silently
 * attributes a random unrelated task's scheduling data to whatever
 * monitored thread happens to share its numeric TID.
 */
typedef __u32 aurora_bpf_tid_t;

#endif /* AURORA_THREAD_EBPF_H */
