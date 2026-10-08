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
 * aurora_thread_stats.c — Thread runtime statistics computation.
 *
 * Implements:
 *   - Procedure-1: Total runtime from exponential histogram
 *   - Derived statistics: mean, range, variance, std_deviation, skewness, kurtosis
 *   - Outlier detection: counts outside mean ± 2σ
 *   - aurora_thread_record_quantum(): histogram update for a single quantum
 *   - aurora_thread_compute_stats(): full stats recomputation for one slot
 *   - aurora_thread_compute_all_stats(): iterate all valid slots
 *
 * All arithmetic uses double precision to avoid overflow for large histograms.
 */

#include "aurora_thread.h"
#include <math.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------- */

/*
 * aurora_thread_log2_bin — compute exponential histogram bin for runtime_ns.
 *
 * Returns i such that runtime_ns ∈ [2^i, 2^(i+1) - 1].
 * For runtime_ns == 0, returns 0 (bin 0 covers [1,1] ns).
 * For runtime_ns > 2^31, returns AURORA_THREAD_HIST_BINS (overflow).
 */
static int aurora_thread_log2_bin(uint64_t runtime_ns)
{
    if (runtime_ns == 0)
        return 0;

    int bin = 0;
    uint64_t v = runtime_ns;

    /* Unrolled bit-scan: find position of the highest set bit */
    if (v >= (1ULL << 32)) return AURORA_THREAD_HIST_BINS; /* overflow */
    if (v >= (1ULL << 16)) { bin += 16; v >>= 16; }
    if (v >= (1ULL <<  8)) { bin +=  8; v >>=  8; }
    if (v >= (1ULL <<  4)) { bin +=  4; v >>=  4; }
    if (v >= (1ULL <<  2)) { bin +=  2; v >>=  2; }
    if (v >= (1ULL <<  1)) { bin +=  1; }

    if (bin >= AURORA_THREAD_HIST_BINS)
        return AURORA_THREAD_HIST_BINS; /* overflow */
    return bin;
}

/*
 * Procedure-1: Calculate total runtime from exponential histogram.
 *
 * In the exponential histogram, bin[i] covers [2^i, 2^(i+1) - 1] ns.
 * Assuming values are uniformly distributed within each bucket:
 *
 *   sum_in_bin  = items_per_bin × (range_start + range_end) × range_size / 2
 *   total       = Σ sum_in_bin for all non-empty bins
 *
 * where items_per_bin = bin_count / range_size (uniform density).
 *
 * This matches the Python reference:
 *   def calc_runtime(bin):
 *       total = 0
 *       for i, bin_count in enumerate(bin):
 *           if bin_count > 0:
 *               range_start = 2 ** i
 *               range_end   = 2 ** (i + 1) - 1
 *               range_size  = range_end - range_start + 1
 *               items_per_bin = bin_count / range_size
 *               total += items_per_bin * ((range_start + range_end) * range_size / 2)
 *       return total
 */
static double aurora_calc_total_runtime(const aurora_thread_hist_t *h)
{
    double total = 0.0;
    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++) {
        if (h->bin[i] == 0)
            continue;
        double range_start   = (double)(1ULL << i);
        double range_end     = (double)((1ULL << (i + 1)) - 1);
        double range_size    = range_end - range_start + 1.0;
        double items_per_bin = (double)h->bin[i] / range_size;
        total += items_per_bin * ((range_start + range_end) * range_size / 2.0);
    }
    /* Add overflow contribution: approximate midpoint 3 × 2^31 ns */
    if (h->overflow > 0) {
        double range_start = (double)(1ULL << AURORA_THREAD_HIST_BINS);
        double approx_mid  = range_start * 3.0; /* very rough estimate */
        total += (double)h->overflow * approx_mid;
    }
    return total;
}

/*
 * Estimate the minimum value from the histogram.
 * Returns the midpoint of the first non-empty bin.
 */
static double aurora_est_min(const aurora_thread_hist_t *h)
{
    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++) {
        if (h->bin[i] > 0) {
            double lo = (double)(1ULL << i);
            double hi = (double)((1ULL << (i + 1)) - 1);
            return (lo + hi) / 2.0;
        }
    }
    return 0.0;
}

/*
 * Return the midpoint (representative value) of bin i.
 */
static inline double bin_midpoint(int i)
{
    double lo = (double)(1ULL << i);
    double hi = (double)((1ULL << (i + 1)) - 1);
    return (lo + hi) / 2.0;
}

/* ---------------------------------------------------------------
 * Public functions
 * --------------------------------------------------------------- */

/*
 * aurora_thread_record_quantum — record a single scheduling quantum.
 *
 * Updates:
 *   - hist_runtime bin
 *   - total_events
 *   - max_runtime_ns
 */
void aurora_thread_record_quantum(aurora_thread_metrics_t *m, uint64_t runtime_ns)
{
    int bin = aurora_thread_log2_bin(runtime_ns);
    if (bin >= AURORA_THREAD_HIST_BINS) {
        m->hist_runtime.overflow++;
    } else {
        m->hist_runtime.bin[bin]++;
    }
    m->total_events++;
    if (runtime_ns > m->max_runtime_ns)
        m->max_runtime_ns = runtime_ns;
}

/*
 * aurora_thread_compute_stats — recompute all derived statistics for one slot.
 *
 * Computes from hist_runtime:
 *   total_runtime_ns  (Procedure-1)
 *   mean_ns
 *   range_ns          (max - est_min)
 *   variance_ns2      (2nd central moment)
 *   std_deviation_ns  (sqrt variance)
 *   skewness          (3rd standardised moment)
 *   kurtosis          (excess: 4th moment / σ⁴ - 3)
 *   outliers_low/high (counts outside mean ± 2σ)
 */
void aurora_thread_compute_stats(aurora_thread_metrics_t *m)
{
    const aurora_thread_hist_t *h = &m->hist_runtime;
    uint64_t N = m->total_events;

    if (N == 0) {
        m->total_runtime_ns = 0.0;
        m->mean_ns          = 0.0;
        m->range_ns         = 0.0;
        m->variance_ns2     = 0.0;
        m->std_deviation_ns = 0.0;
        m->skewness         = 0.0;
        m->kurtosis         = 0.0;
        m->outliers_low     = 0;
        m->outliers_high    = 0;
        return;
    }

    /* Total runtime via Procedure-1 */
    m->total_runtime_ns = aurora_calc_total_runtime(h);

    /* Mean */
    m->mean_ns = m->total_runtime_ns / (double)N;

    /* Range: max - estimated minimum */
    m->range_ns = (double)m->max_runtime_ns - aurora_est_min(h);
    if (m->range_ns < 0.0)
        m->range_ns = 0.0;

    /* Central moments m2, m3, m4 — single pass over non-empty bins */
    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    double lo_bound, hi_bound;

    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++) {
        if (h->bin[i] == 0)
            continue;
        double mid = bin_midpoint(i);
        double dev = mid - m->mean_ns;
        double cnt = (double)h->bin[i];
        double d2  = dev * dev;
        m2 += cnt * d2;
        m3 += cnt * d2 * dev;
        m4 += cnt * d2 * d2;
    }
    /* Include overflow bin: use approximate midpoint 3 × 2^HIST_BINS ns */
    if (h->overflow > 0) {
        double mid = (double)(1ULL << AURORA_THREAD_HIST_BINS) * 3.0;
        double dev = mid - m->mean_ns;
        double cnt = (double)h->overflow;
        double d2  = dev * dev;
        m2 += cnt * d2;
        m3 += cnt * d2 * dev;
        m4 += cnt * d2 * d2;
    }

    m->variance_ns2    = m2 / (double)N;
    m->std_deviation_ns = sqrt(m->variance_ns2);

    double sigma3 = m->std_deviation_ns * m->std_deviation_ns * m->std_deviation_ns;
    double sigma4 = sigma3 * m->std_deviation_ns;

    m->skewness = (sigma3 > 0.0) ? (m3 / (double)N) / sigma3 : 0.0;
    m->kurtosis = (sigma4 > 0.0) ? (m4 / (double)N) / sigma4 - 3.0 : 0.0;

    /* Outlier bounds: mean ± 2σ */
    lo_bound = m->mean_ns - 2.0 * m->std_deviation_ns;
    hi_bound = m->mean_ns + 2.0 * m->std_deviation_ns;

    m->outliers_low  = 0;
    m->outliers_high = 0;
    for (int i = 0; i < AURORA_THREAD_HIST_BINS; i++) {
        if (h->bin[i] == 0)
            continue;
        double mid = bin_midpoint(i);
        if (mid < lo_bound)
            m->outliers_low  += h->bin[i];
        else if (mid > hi_bound)
            m->outliers_high += h->bin[i];
    }
    /* Overflow bin */
    if (h->overflow > 0) {
        double mid = (double)(1ULL << AURORA_THREAD_HIST_BINS) * 3.0;
        if (mid > hi_bound)
            m->outliers_high += h->overflow;
    }
}

/*
 * aurora_thread_compute_all_stats — recompute derived stats for all valid slots.
 */
void aurora_thread_compute_all_stats(aurora_thread_stats_t *ts)
{
    uint32_t n = atomic_load_explicit(&ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < AURORA_MAX_MONITORED_THREADS; i++) {
        if (ts->threads[i].valid)
            aurora_thread_compute_stats(&ts->threads[i]);
    }
}

/*
 * aurora_thread_reset_interval_stats — zero per-interval counters and histograms.
 *
 * Preserves: valid, pid, tid, process_name, thread_name, _prev_* delta state.
 * Zeroes: total_events, max_runtime_ns, hist_runtime, all derived stats.
 */
void aurora_thread_reset_interval_stats(aurora_thread_stats_t *ts)
{
    uint32_t n = atomic_load_explicit(&ts->num_threads, memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < AURORA_MAX_MONITORED_THREADS; i++) {
        aurora_thread_metrics_t *m = &ts->threads[i];
        if (!m->valid)
            continue;

        m->total_events = 0;
        m->max_runtime_ns = 0;
        memset(&m->hist_runtime, 0, sizeof(m->hist_runtime));

        m->total_runtime_ns = 0.0;
        m->mean_ns          = 0.0;
        m->range_ns         = 0.0;
        m->variance_ns2     = 0.0;
        m->std_deviation_ns = 0.0;
        m->skewness         = 0.0;
        m->kurtosis         = 0.0;
        m->outliers_low     = 0;
        m->outliers_high    = 0;

        m->irq_count         = 0;
        m->voluntary_ctxsw   = 0;
        m->involuntary_ctxsw = 0;
        m->cpu_migrations    = 0;
    }
}
