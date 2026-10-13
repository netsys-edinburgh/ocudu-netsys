/*
 * Per-interval merge of the eBPF thread statistics into shared memory.
 *
 * Kept apart from aurora_thread_ebpf.c, which needs libbpf and the generated
 * BPF skeleton, so that it is always built and unit-testable without loading
 * any BPF program.
 */

#ifndef AURORA_THREAD_EBPF_MERGE_H
#define AURORA_THREAD_EBPF_MERGE_H

#include "aurora_thread.h"
#include "aurora_thread_ebpf.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * aurora_ebpf_merge_interval — add the activity of one thread since the
 * previous collection to its shared-memory slot.
 *
 * @m:            shared-memory slot of the thread.
 * @prev:         totals read from tid_stats at the previous collection, all
 *                zero for a thread not read before. Replaced by @now.
 * @now:          totals just read from tid_stats (cumulative counters).
 * @closed_epoch: interval number the collection closes. The BPF per-interval
 *                maximum is only used when it belongs to this interval;
 *                otherwise the thread did not run during it.
 *
 * Counters lower than in @prev mean the BPF entry was recreated (the thread
 * exited and its TID was reused), so the deltas restart from zero.
 */
void aurora_ebpf_merge_interval(aurora_thread_metrics_t                *m,
                                struct aurora_bpf_thread_stats         *prev,
                                const struct aurora_bpf_thread_stats   *now,
                                __u64                                   closed_epoch);

#ifdef __cplusplus
}
#endif

#endif /* AURORA_THREAD_EBPF_MERGE_H */
