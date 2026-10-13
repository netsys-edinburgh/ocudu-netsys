/*
 * Per-interval merge of the eBPF thread statistics into shared memory
 * (see aurora_thread_ebpf_merge.h).
 */

#include "aurora_thread_ebpf_merge.h"

#include <string.h>

/* Returns 1 when any cumulative counter of @now is below @prev, i.e. the BPF entry was recreated. */
static int aurora_ebpf_counters_went_back(const struct aurora_bpf_thread_stats *prev,
                                          const struct aurora_bpf_thread_stats *now)
{
    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++)
        if (now->hist_runtime[i] < prev->hist_runtime[i])
            return 1;
    return now->overflow < prev->overflow || now->total_events < prev->total_events ||
           now->irq_count < prev->irq_count || now->voluntary_ctxsw < prev->voluntary_ctxsw ||
           now->involuntary_ctxsw < prev->involuntary_ctxsw || now->cpu_migrations < prev->cpu_migrations;
}

void aurora_ebpf_merge_interval(aurora_thread_metrics_t                *m,
                                struct aurora_bpf_thread_stats         *prev,
                                const struct aurora_bpf_thread_stats   *now,
                                __u64                                   closed_epoch)
{
    if (aurora_ebpf_counters_went_back(prev, now))
        memset(prev, 0, sizeof(*prev));

    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++)
        m->hist_runtime.bin[i] += now->hist_runtime[i] - prev->hist_runtime[i];
    m->hist_runtime.overflow += now->overflow - prev->overflow;
    m->total_events          += now->total_events - prev->total_events;
    m->irq_count             += now->irq_count - prev->irq_count;
    m->voluntary_ctxsw       += now->voluntary_ctxsw - prev->voluntary_ctxsw;
    m->involuntary_ctxsw     += now->involuntary_ctxsw - prev->involuntary_ctxsw;
    m->cpu_migrations        += now->cpu_migrations - prev->cpu_migrations;

    /* A cumulative maximum says nothing about one interval: use the BPF
     * per-interval maximum, if it belongs to the interval being closed. */
    if (now->interval_epoch == closed_epoch && now->interval_max_runtime_ns > m->max_runtime_ns)
        m->max_runtime_ns = now->interval_max_runtime_ns;

    /* last_cpu is a gauge, not a counter. */
    if (now->last_cpu != AURORA_BPF_CPU_UNSET)
        m->cpu_id = (int)now->last_cpu;

    *prev = *now;
}
