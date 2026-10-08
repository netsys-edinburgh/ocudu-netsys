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
 * aurora_reader — continuous shared-memory reader with CSV export.
 *
 * Modes:
 *   Default  : poll shared memory every <interval> ms, buffer 100 samples,
 *              flush one CSV row per sample to <output_file> every 100 samples.
 *   --once   : read once, print formatted summary, exit (legacy behaviour).
 *
 * CSV layout:
 *   - One row per sample (one collection interval).
 *   - timestamp_us is the index column.
 *   - Per-UE metrics (MAC / RLC / PDCP / GTP) are pre-aggregated across all
 *     active UEs / bearers / tunnels into _min, _max, _avg triplets so that
 *     each metric group contributes exactly three columns.
 *   - Per-cell RC metrics are likewise reduced to scalar aggregates.
 */

#include "aurora_shm.h"
#include "aurora_infra.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <signal.h>
#include <time.h>
#include <sys/types.h>

/* =========================================================
 * Histogram statistics engine (Procedure-1 and Procedure-2)
 * ========================================================= */

static const uint32_t HIST_BIN_LOWER[AURORA_HIST_BINS] = {
    0, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576
};
static const uint32_t HIST_BIN_UPPER[AURORA_HIST_BINS] = {
    63, 127, 255, 511, 1023, 2047, 4095, 8191,
    16383, 32767, 65535, 131071, 262143, 524287, 1048575, 2097151
};

/* Procedure-1: exact mean, tracked alongside the histogram (aurora_hist_t.sum).
 * A bin-midpoint mean derived from bins[] alone can fall outside
 * [min_val, max_val] when few samples land in a wide high-numbered bin,
 * since every sample in a bin is represented by that bin's midpoint
 * regardless of where within the bin it actually falls. Exact sum/count
 * avoids that. */
static double hist_mean_exp(const aurora_hist_t *hist) {
    return hist->count > 0 ? (double)hist->sum / (double)hist->count : 0.0;
}

/* Procedure-1: full statistical variants (min/max/range/IQR/variance/
 * std_dev/skewness/kurtosis/outliers) derived from an exponential
 * packet-size histogram. See AURORA.md "Statistical Variants". */
typedef struct {
    double   min, max, mean, range, iqr, variance, std_dev, skewness, kurtosis;
    uint32_t outliers;
} hist_stats_t;

static double hist_percentile_exp(const uint32_t *bins, uint32_t count, double pct) {
    if (count == 0) return 0.0;
    double target = pct * (double)count;
    uint32_t cum = 0;
    for (int i = 0; i < AURORA_HIST_BINS; i++) {
        if (bins[i] == 0) continue;
        uint32_t next_cum = cum + bins[i];
        if ((double)next_cum >= target) {
            double lo = (double)HIST_BIN_LOWER[i];
            double hi = (double)HIST_BIN_UPPER[i];
            double frac = (target - cum) / (double)bins[i];
            return lo + frac * (hi - lo);
        }
        cum = next_cum;
    }
    return (double)HIST_BIN_UPPER[AURORA_HIST_BINS - 1];
}

static void hist_stats_exp(const aurora_hist_t *hist, hist_stats_t *st) {
    memset(st, 0, sizeof(*st));
    if (hist->count == 0) return;

    double mean = hist_mean_exp(hist);
    st->min   = (double)hist->min_val;
    st->max   = (double)hist->max_val;
    st->mean  = mean;
    st->range = st->max - st->min;

    double m2 = 0.0, m3 = 0.0, m4 = 0.0;
    for (int i = 0; i < AURORA_HIST_BINS; i++) {
        if (hist->bins[i] == 0) continue;
        double mid  = ((double)HIST_BIN_LOWER[i] + (double)HIST_BIN_UPPER[i]) / 2.0;
        double dev  = mid - mean;
        double dev2 = dev * dev;
        m2 += hist->bins[i] * dev2;
        m3 += hist->bins[i] * dev2 * dev;
        m4 += hist->bins[i] * dev2 * dev2;
    }
    st->variance = m2 / (double)hist->count;
    st->std_dev  = sqrt(st->variance);

    double sigma3 = st->std_dev * st->std_dev * st->std_dev;
    double sigma4 = sigma3 * st->std_dev;
    st->skewness = (sigma3 > 0.0) ? (m3 / (double)hist->count) / sigma3 : 0.0;
    st->kurtosis = (sigma4 > 0.0) ? (m4 / (double)hist->count) / sigma4 - 3.0 : 0.0;

    double q1 = hist_percentile_exp(hist->bins, hist->count, 0.25);
    double q3 = hist_percentile_exp(hist->bins, hist->count, 0.75);
    st->iqr = q3 - q1;

    double lo_bound = mean - 2.0 * st->std_dev;
    double hi_bound = mean + 2.0 * st->std_dev;
    for (int i = 0; i < AURORA_HIST_BINS; i++) {
        if (hist->bins[i] == 0) continue;
        double mid = ((double)HIST_BIN_LOWER[i] + (double)HIST_BIN_UPPER[i]) / 2.0;
        if (mid < lo_bound || mid > hi_bound)
            st->outliers += hist->bins[i];
    }
}

/* Procedure-2: exact mean for a shift-type (fixed-width-bucket) histogram,
 * from aurora_hist_t.sum (same reasoning as hist_mean_exp above -- a
 * bucket-midpoint approximation is still only an approximation even when
 * clamped to [min_val, max_val]; the exact sum is free). */
static double hist_mean_shift(const aurora_hist_t *hist, int shift) {
    (void)shift;
    return hist->count > 0 ? (double)hist->sum / (double)hist->count : 0.0;
}

/* =========================================================
 * Per-UE aggregate helper
 * ========================================================= */

typedef struct { double min, max, sum; int n; } agg_t;

/* =========================================================
 * Thread CSV layout — built once at write_csv_header time.
 * Maps each CSV column slot to a thread slot index + name.
 * ========================================================= */

typedef struct {
    uint64_t total_events;
    uint64_t max_runtime_ns;
    double   total_runtime_ns;
    double   mean_ns;
    double   range_ns;
    double   std_deviation_ns;
    double   skewness;
    double   kurtosis;
    uint64_t outliers_low;
    uint64_t outliers_high;
    uint64_t irq_count;
    uint64_t voluntary_ctxsw;
    uint64_t involuntary_ctxsw;
    uint64_t cpu_migrations;
    int      cpu_id;
} thread_snap_t;

typedef struct {
    int  slot_idx;                           /* index in thread_stats.threads[] */
    char name[AURORA_THREAD_NAME_LEN];       /* original thread name */
    char safe_name[AURORA_THREAD_NAME_LEN];  /* sanitised for CSV column names */
} thread_csv_col_t;

static thread_csv_col_t g_thread_cols[AURORA_MAX_MONITORED_THREADS];
static int               g_thread_ncols = 0;

/* Infrastructure metrics (Option B — separate SHM) */
static aurora_infra_metrics_t *g_infra_metrics = NULL;
static aurora_infra_config_t   g_infra_cfg;

/* Replace any character that is not alphanumeric or '_' with '_'. */
static void sanitise_thread_name(const char *src, char *dst, size_t len)
{
    size_t i;
    for (i = 0; i < len - 1 && src[i] != '\0'; i++)
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ||
                 (src[i] >= 'A' && src[i] <= 'Z') ||
                 (src[i] >= '0' && src[i] <= '9') ? src[i] : '_';
    dst[i] = '\0';
}

static void agg_add(agg_t *a, double v) {
    if (a->n == 0 || v < a->min) a->min = v;
    if (a->n == 0 || v > a->max) a->max = v;
    a->sum += v;
    a->n++;
}
static double agg_avg(const agg_t *a) { return a->n > 0 ? a->sum / a->n : 0.0; }

/* =========================================================
 * Snapshot — all metrics for one collection interval,
 * with per-UE metrics pre-aggregated to min/max/avg.
 * ========================================================= */

typedef struct {
    /* Index */
    uint64_t timestamp_us;
    ran_type_t ran_type;  /* carried through for write_csv_row's column gating --
                            * must match write_csv_header's gating on the same role */

    /* Group 1 — Traffic flows: bytes, packet count, mean packet size,
     * plus full Procedure-1 statistical variants per flow. */
    uint64_t bhtx_in_bytes;   uint32_t bhtx_in_pkts;   double bhtx_in_pkt_mean;   hist_stats_t bhtx_in_stats;
    uint64_t bhtx_out_bytes;  uint32_t bhtx_out_pkts;  double bhtx_out_pkt_mean;  hist_stats_t bhtx_out_stats;
    uint64_t bhrx_in_bytes;   uint32_t bhrx_in_pkts;   double bhrx_in_pkt_mean;   hist_stats_t bhrx_in_stats;
    uint64_t bhrx_out_bytes;  uint32_t bhrx_out_pkts;  double bhrx_out_pkt_mean;  hist_stats_t bhrx_out_stats;
    uint64_t mhtx_in_bytes;   uint32_t mhtx_in_pkts;   double mhtx_in_pkt_mean;   hist_stats_t mhtx_in_stats;
    uint64_t mhtx_out_bytes;  uint32_t mhtx_out_pkts;  double mhtx_out_pkt_mean;  hist_stats_t mhtx_out_stats;
    uint64_t mhrx_in_bytes;   uint32_t mhrx_in_pkts;   double mhrx_in_pkt_mean;   hist_stats_t mhrx_in_stats;
    uint64_t mhrx_out_bytes;  uint32_t mhrx_out_pkts;  double mhrx_out_pkt_mean;  hist_stats_t mhrx_out_stats;
    uint64_t f1u_rlc_bytes;   uint32_t f1u_rlc_pkts;   double f1u_rlc_pkt_mean;   hist_stats_t f1u_rlc_stats;
    uint64_t rlc_f1_bytes;    uint32_t rlc_f1_pkts;    double rlc_f1_pkt_mean;    hist_stats_t rlc_f1_stats;
    uint64_t rlc_mac_bytes;   uint32_t rlc_mac_pkts;   double rlc_mac_pkt_mean;   hist_stats_t rlc_mac_stats;
    uint64_t mac_rlc_bytes;   uint32_t mac_rlc_pkts;   double mac_rlc_pkt_mean;   hist_stats_t mac_rlc_stats;
    uint64_t mac_dl_bo;       uint32_t mac_dl_bo_smpl; double mac_dl_bo_mean;     hist_stats_t mac_dl_bo_stats;

    /* Group 2 — HARQ (DL) */
    uint64_t dl_harq_ack;
    uint64_t dl_harq_nack;
    uint64_t dl_harq_total;
    double   dl_harq_loss_rate;
    uint32_t dl_harq_max_cons;

    /* Group 3 — CRC (UL) */
    uint64_t ul_crc_loss;
    uint64_t ul_crc_total;
    double   ul_crc_loss_rate;

    /* Group 4 — SINR / CSI / BSR aggregates */
    float    sinr_min, sinr_max, sinr_avg;
    uint8_t  csi_min,  csi_max;
    float    csi_avg;
    uint32_t bsr_min,  bsr_max;
    float    bsr_avg;

    /* Group 5 — FAPI DL */
    uint32_t fapi_dl_pdsch_count;
    float    fapi_dl_avg_pdsch;
    uint8_t  dl_fapi_mcs_max, dl_fapi_mcs_min;
    float    dl_fapi_mcs_avg;
    double   dl_fapi_mcs_hist_avg;
    uint32_t dl_fapi_prb_max, dl_fapi_prb_min;
    float    dl_fapi_prb_avg;
    double   dl_fapi_prb_hist_avg;
    uint32_t dl_fapi_tbs_max, dl_fapi_tbs_min;
    float    dl_fapi_tbs_avg;

    /* Group 6 — FAPI UL */
    uint32_t fapi_ul_pusch_count;
    float    fapi_ul_avg_pusch;
    uint8_t  ul_fapi_mcs_max, ul_fapi_mcs_min;
    float    ul_fapi_mcs_avg;
    double   ul_fapi_mcs_hist_avg;
    uint32_t ul_fapi_prb_max, ul_fapi_prb_min;
    float    ul_fapi_prb_avg;
    double   ul_fapi_prb_hist_avg;
    uint32_t ul_fapi_tbs_max, ul_fapi_tbs_min;
    float    ul_fapi_tbs_avg;

    /* FAPI UE count */
    uint32_t fapi_ue_count;

    /* Group 7 — Per-UE FAPI metrics (aggregated across active UEs) */
    int    fapi_active_ues;
    double fapi_ue_dl_mcs_min, fapi_ue_dl_mcs_max, fapi_ue_dl_mcs_avg;
    double fapi_ue_dl_prb_min, fapi_ue_dl_prb_max, fapi_ue_dl_prb_avg;
    double fapi_ue_dl_tbs_min, fapi_ue_dl_tbs_max, fapi_ue_dl_tbs_avg;
    double fapi_ue_ul_mcs_min, fapi_ue_ul_mcs_max, fapi_ue_ul_mcs_avg;
    double fapi_ue_ul_prb_min, fapi_ue_ul_prb_max, fapi_ue_ul_prb_avg;
    double fapi_ue_ul_tbs_min, fapi_ue_ul_tbs_max, fapi_ue_ul_tbs_avg;

    /* Group 8 — KPM (12 standard O-RAN + 4 locally-defined cell-level measurements) */
    double kpm[16];  /* index matches measurement_id - 1 */

    /* Group 12 — Per-UE MAC (aggregated across active UEs) */
    int    mac_active_ues;
    double mac_dl_tbs_min,    mac_dl_tbs_max,    mac_dl_tbs_avg;
    double mac_ul_tbs_min,    mac_ul_tbs_max,    mac_ul_tbs_avg;
    double mac_dl_prb_min,    mac_dl_prb_max,    mac_dl_prb_avg;
    double mac_ul_prb_min,    mac_ul_prb_max,    mac_ul_prb_avg;
    double mac_dl_mcs_min,    mac_dl_mcs_max,    mac_dl_mcs_avg;
    double mac_ul_mcs_min,    mac_ul_mcs_max,    mac_ul_mcs_avg;
    double mac_pusch_snr_min, mac_pusch_snr_max, mac_pusch_snr_avg;
    double mac_pucch_snr_min, mac_pucch_snr_max, mac_pucch_snr_avg;
    double mac_dl_bler_min,   mac_dl_bler_max,   mac_dl_bler_avg;
    double mac_ul_bler_min,   mac_ul_bler_max,   mac_ul_bler_avg;
    double mac_cqi_min,       mac_cqi_max,       mac_cqi_avg;
    double mac_phr_min,       mac_phr_max,       mac_phr_avg;
    double mac_bsr_min,       mac_bsr_max,       mac_bsr_avg;

    /* Group 13 — Per-UE RLC (aggregated across active bearers) */
    int    rlc_active_bearers;
    double rlc_txpdu_bytes_min, rlc_txpdu_bytes_max, rlc_txpdu_bytes_avg;
    double rlc_rxpdu_bytes_min, rlc_rxpdu_bytes_max, rlc_rxpdu_bytes_avg;
    double rlc_txsdu_bytes_min, rlc_txsdu_bytes_max, rlc_txsdu_bytes_avg;
    double rlc_rxsdu_bytes_min, rlc_rxsdu_bytes_max, rlc_rxsdu_bytes_avg;
    double rlc_lost_pkts_min,   rlc_lost_pkts_max,   rlc_lost_pkts_avg;
    double rlc_hol_us_min,      rlc_hol_us_max,      rlc_hol_us_avg;
    double rlc_rx_hol_us_min,   rlc_rx_hol_us_max,   rlc_rx_hol_us_avg;
    double rlc_retx_depth_min,  rlc_retx_depth_max,  rlc_retx_depth_avg;
    double rlc_stall_events_min, rlc_stall_events_max, rlc_stall_events_avg;

    /* Group 14 — Per-UE PDCP (aggregated across active bearers) */
    int    pdcp_active_bearers;
    double pdcp_txpdu_bytes_min, pdcp_txpdu_bytes_max, pdcp_txpdu_bytes_avg;
    double pdcp_rxpdu_bytes_min, pdcp_rxpdu_bytes_max, pdcp_rxpdu_bytes_avg;
    double pdcp_txsdu_bytes_min, pdcp_txsdu_bytes_max, pdcp_txsdu_bytes_avg;
    double pdcp_rxsdu_bytes_min, pdcp_rxsdu_bytes_max, pdcp_rxsdu_bytes_avg;
    double pdcp_lost_pkts_min,   pdcp_lost_pkts_max,   pdcp_lost_pkts_avg;
    double pdcp_tx_proc_latency_us_min, pdcp_tx_proc_latency_us_max, pdcp_tx_proc_latency_us_avg;

    /* Group 15 — Per-UE GTP (aggregated across active tunnels) */
    int    gtp_active_tunnels;
    double gtp_tx_bytes_min, gtp_tx_bytes_max, gtp_tx_bytes_avg;
    double gtp_rx_bytes_min, gtp_rx_bytes_max, gtp_rx_bytes_avg;
    double gtp_tx_err_min,   gtp_tx_err_max,   gtp_tx_err_avg;
    double gtp_rx_err_min,   gtp_rx_err_max,   gtp_rx_err_avg;
    double gtp_tx_proc_latency_us_min, gtp_tx_proc_latency_us_max, gtp_tx_proc_latency_us_avg;

    /* Group 11 — RC cell (cell 0 = primary serving cell) */
    int      rc_cell_count;
    double   rc0_rsrp_avg;
    double   rc0_sinr_avg;
    uint32_t rc0_cell_load_pct;
    uint32_t rc0_interference_lvl;
    uint32_t rc0_ho_attempts;
    uint32_t rc0_ho_success;
    double   rc0_ho_success_rate;

    /* UE churn (deltas per interval): metrics->ue_attach_count/ue_detach_count
     * are reset every interval by aurora_reset_thread, so a poller must read
     * every interval to observe attach/detach events -- a skipped interval's
     * counts are lost rather than accumulated into the next one. */
    uint32_t ue_attach_count;
    uint32_t ue_detach_count;

    /* CU-side: number of DUs currently connected (gauge, not a per-interval
     * delta -- mirrors RRC's own rrc->num_dus, updated at rrc_add_du()/
     * rrc_rm_du() via aurora_update_du_count(), not touched by
     * aurora_reset_thread). */
    uint32_t du_count;

    /* Group 16 — Process resource usage (deltas per interval) */
    uint64_t ru_utime_us;
    uint64_t ru_stime_us;
    uint64_t ru_maxrss_kb;
    uint64_t ru_minflt;
    uint64_t ru_majflt;
    uint64_t ru_inblock;
    uint64_t ru_oublock;
    uint64_t ru_nvcsw;
    uint64_t ru_nivcsw;

    /* Group 17 — Per-thread runtime metrics (indexed by g_thread_cols layout) */
    thread_snap_t thread_snaps[AURORA_MAX_MONITORED_THREADS];
} snapshot_t;

/* =========================================================
 * fill_snapshot — extract one snapshot from shared memory.
 * Per-UE arrays are reduced to min/max/avg in-place.
 * ========================================================= */

static void fill_snapshot(snapshot_t *s, const struct aurora_metrics *m)
{
    memset(s, 0, sizeof(*s));
    s->timestamp_us = m->timestamp_us;
    s->ran_type = m->ran_type;

    /* --- Group 1: traffic flows --- */
#define FILL_FLOW(field, hist_field) \
    s->field##_bytes    = m->field##_size; \
    s->field##_pkts     = m->hist_field.count; \
    s->field##_pkt_mean = hist_mean_exp(&m->hist_field); \
    hist_stats_exp(&m->hist_field, &s->field##_stats)

    FILL_FLOW(bhtx_in,  hist_bhtx_in);
    FILL_FLOW(bhtx_out, hist_bhtx_out);
    FILL_FLOW(bhrx_in,  hist_bhrx_in);
    FILL_FLOW(bhrx_out, hist_bhrx_out);
    FILL_FLOW(mhtx_in,  hist_mhtx_in);
    FILL_FLOW(mhtx_out, hist_mhtx_out);
    FILL_FLOW(mhrx_in,  hist_mhrx_in);
    FILL_FLOW(mhrx_out, hist_mhrx_out);
    FILL_FLOW(f1u_rlc,  hist_f1u_rlc);
    FILL_FLOW(rlc_f1,   hist_rlc_f1);
    FILL_FLOW(rlc_mac,  hist_rlc_mac);
    FILL_FLOW(mac_rlc,  hist_mac_rlc);
#undef FILL_FLOW

    s->mac_dl_bo      = m->mac_dl_bo;
    s->mac_dl_bo_smpl = m->hist_mac_dl_bo.count;
    s->mac_dl_bo_mean = hist_mean_exp(&m->hist_mac_dl_bo);
    hist_stats_exp(&m->hist_mac_dl_bo, &s->mac_dl_bo_stats);

    /* --- Group 2: HARQ --- */
    s->dl_harq_ack       = m->dl_harq_ack;
    s->dl_harq_nack      = m->dl_harq_nack;
    s->dl_harq_total     = m->dl_harq_total;
    s->dl_harq_loss_rate = m->dl_harq_loss_rate;
    s->dl_harq_max_cons  = m->dl_harq_max_cons;

    /* --- Group 3: CRC --- */
    s->ul_crc_loss      = m->ul_crc_loss;
    s->ul_crc_total     = m->ul_crc_total;
    s->ul_crc_loss_rate = m->ul_crc_loss_rate;

    /* --- Group 4: SINR/CSI/BSR --- */
    s->sinr_min = m->sinr_min;
    s->sinr_max = m->sinr_max;
    s->sinr_avg = m->sinr_avg;
    s->csi_min  = m->csi_min;
    s->csi_max  = m->csi_max;
    s->csi_avg  = m->csi_avg;
    s->bsr_min  = m->bsr_min;
    s->bsr_max  = m->bsr_max;
    s->bsr_avg  = m->bsr_avg;

    /* --- Group 5: FAPI DL --- */
    s->fapi_dl_pdsch_count = m->fapi_dl_total_pdsch_count;
    s->fapi_dl_avg_pdsch   = m->fapi_dl_avg_pdsch;
    s->dl_fapi_mcs_max     = m->dl_fapi_mcs_max;
    s->dl_fapi_mcs_min     = (m->dl_fapi_mcs_min == 0xFF) ? 0 : m->dl_fapi_mcs_min;
    s->dl_fapi_mcs_avg     = m->dl_fapi_mcs_avg;
    s->dl_fapi_mcs_hist_avg = hist_mean_shift(&m->hist_dl_fapi_mcs, 2);
    s->dl_fapi_prb_max     = m->dl_fapi_prb_max;
    s->dl_fapi_prb_min     = (m->dl_fapi_prb_min == 0xFFFFFFFF) ? 0 : m->dl_fapi_prb_min;
    s->dl_fapi_prb_avg     = m->dl_fapi_prb_avg;
    s->dl_fapi_prb_hist_avg = hist_mean_shift(&m->hist_dl_fapi_prb, 5);
    s->dl_fapi_tbs_max     = m->dl_fapi_tbs_max;
    s->dl_fapi_tbs_min     = (m->dl_fapi_tbs_min == 0xFFFFFFFF) ? 0 : m->dl_fapi_tbs_min;
    s->dl_fapi_tbs_avg     = m->dl_fapi_tbs_avg;

    /* --- Group 6: FAPI UL --- */
    s->fapi_ul_pusch_count = m->fapi_ul_total_pusch_count;
    s->fapi_ul_avg_pusch   = m->fapi_ul_avg_pusch;
    s->ul_fapi_mcs_max     = m->ul_fapi_mcs_max;
    s->ul_fapi_mcs_min     = (m->ul_fapi_mcs_min == 0xFF) ? 0 : m->ul_fapi_mcs_min;
    s->ul_fapi_mcs_avg     = m->ul_fapi_mcs_avg;
    s->ul_fapi_mcs_hist_avg = hist_mean_shift(&m->hist_ul_fapi_mcs, 2);
    s->ul_fapi_prb_max     = m->ul_fapi_prb_max;
    s->ul_fapi_prb_min     = (m->ul_fapi_prb_min == 0xFFFFFFFF) ? 0 : m->ul_fapi_prb_min;
    s->ul_fapi_prb_avg     = m->ul_fapi_prb_avg;
    s->ul_fapi_prb_hist_avg = hist_mean_shift(&m->hist_ul_fapi_prb, 5);
    s->ul_fapi_tbs_max     = m->ul_fapi_tbs_max;
    s->ul_fapi_tbs_min     = (m->ul_fapi_tbs_min == 0xFFFFFFFF) ? 0 : m->ul_fapi_tbs_min;
    s->ul_fapi_tbs_avg     = m->ul_fapi_tbs_avg;
    s->fapi_ue_count       = m->fapi_ue_count;

    /* --- Group 7: Per-UE FAPI metrics → aggregate across active UEs --- */
    {
        agg_t dl_mcs={0}, dl_prb={0}, dl_tbs={0};
        agg_t ul_mcs={0}, ul_prb={0}, ul_tbs={0};

        for (int i = 0; i < MAX_NO_UEs; i++) {
            const aurora_fapi_ue_metrics_t *u = &m->fapi_ue_metrics[i];
            if (u->rnti == 0) continue;
            s->fapi_active_ues++;
            agg_add(&dl_mcs, (double)u->dl_mcs);
            agg_add(&dl_prb, (double)u->dl_prb);
            agg_add(&dl_tbs, (double)u->dl_tbs);
            agg_add(&ul_mcs, (double)u->ul_mcs);
            agg_add(&ul_prb, (double)u->ul_prb);
            agg_add(&ul_tbs, (double)u->ul_tbs);
        }
#define EXPAND_AGG3(prefix, a) \
        s->prefix##_min = (a).n > 0 ? (a).min : 0.0; \
        s->prefix##_max = (a).n > 0 ? (a).max : 0.0; \
        s->prefix##_avg = agg_avg(&(a))

        EXPAND_AGG3(fapi_ue_dl_mcs, dl_mcs);
        EXPAND_AGG3(fapi_ue_dl_prb, dl_prb);
        EXPAND_AGG3(fapi_ue_dl_tbs, dl_tbs);
        EXPAND_AGG3(fapi_ue_ul_mcs, ul_mcs);
        EXPAND_AGG3(fapi_ue_ul_prb, ul_prb);
        EXPAND_AGG3(fapi_ue_ul_tbs, ul_tbs);
#undef EXPAND_AGG3
    }

    /* --- Group 8: KPM --- */
    for (uint32_t k = 0; k < m->kpm_metric_count && k < AURORA_MAX_KPM_METRICS; k++) {
        const aurora_kpm_metric_t *km = &m->kpm_metrics[k];
        uint32_t idx = km->measurement_id - 1;
        if (idx < 16)
            s->kpm[idx] = (km->value_type == 0) ? (double)km->value : km->value_real;
    }

    /* --- Group 12: Per-UE MAC → aggregate across active UEs --- */
    {
        agg_t dl_tbs={0}, ul_tbs={0}, dl_prb={0}, ul_prb={0};
        agg_t dl_mcs={0}, ul_mcs={0};
        agg_t pusch_snr={0}, pucch_snr={0};
        agg_t dl_bler={0}, ul_bler={0};
        agg_t cqi={0}, phr={0}, bsr={0};

        for (int i = 0; i < MAX_NO_UEs; i++) {
            const aurora_mac_ue_stats_t *u = &m->mac_ue_stats[i];
            if (u->rnti == 0) continue;
            s->mac_active_ues++;
            agg_add(&dl_tbs,    (double)u->dl_curr_tbs);
            agg_add(&ul_tbs,    (double)u->ul_curr_tbs);
            agg_add(&dl_prb,    (double)u->dl_aggr_prb);
            agg_add(&ul_prb,    (double)u->ul_aggr_prb);
            agg_add(&dl_mcs,    (double)u->dl_mcs1);
            agg_add(&ul_mcs,    (double)u->ul_mcs1);
            agg_add(&pusch_snr, (double)u->pusch_snr);
            agg_add(&pucch_snr, (double)u->pucch_snr);
            agg_add(&dl_bler,   (double)u->dl_bler);
            agg_add(&ul_bler,   (double)u->ul_bler);
            agg_add(&cqi,       (double)u->wb_cqi);
            agg_add(&phr,       (double)u->phr);
            agg_add(&bsr,       (double)u->bsr);
        }
#define EXPAND_AGG3(prefix, a) \
        s->prefix##_min = (a).n > 0 ? (a).min : 0.0; \
        s->prefix##_max = (a).n > 0 ? (a).max : 0.0; \
        s->prefix##_avg = agg_avg(&(a))

        EXPAND_AGG3(mac_dl_tbs,    dl_tbs);
        EXPAND_AGG3(mac_ul_tbs,    ul_tbs);
        EXPAND_AGG3(mac_dl_prb,    dl_prb);
        EXPAND_AGG3(mac_ul_prb,    ul_prb);
        EXPAND_AGG3(mac_dl_mcs,    dl_mcs);
        EXPAND_AGG3(mac_ul_mcs,    ul_mcs);
        EXPAND_AGG3(mac_pusch_snr, pusch_snr);
        EXPAND_AGG3(mac_pucch_snr, pucch_snr);
        EXPAND_AGG3(mac_dl_bler,   dl_bler);
        EXPAND_AGG3(mac_ul_bler,   ul_bler);
        EXPAND_AGG3(mac_cqi,       cqi);
        EXPAND_AGG3(mac_phr,       phr);
        EXPAND_AGG3(mac_bsr,       bsr);
    }

    /* --- Group 13: Per-UE RLC → aggregate across active bearers --- */
    {
        agg_t txpdu_b={0}, rxpdu_b={0}, txsdu_b={0}, rxsdu_b={0};
        agg_t lost={0}, hol={0}, rx_hol={0}, retx_depth={0}, stall={0};

        for (int i = 0; i < MAX_NO_UEs; i++) {
            for (int rb = 0; rb < MAX_RBS_PER_UE; rb++) {
                const aurora_rlc_radio_bearer_stats_t *r = &m->rlc_rb_stats[i][rb];
                if (r->rnti == 0) continue;
                s->rlc_active_bearers++;
                agg_add(&txpdu_b,    (double)r->txpdu_bytes);
                agg_add(&rxpdu_b,    (double)r->rxpdu_bytes);
                agg_add(&txsdu_b,    (double)r->txsdu_bytes);
                agg_add(&rxsdu_b,    (double)r->rxsdu_bytes);
                agg_add(&lost,       (double)r->rxpdu_lost_pkts);
                agg_add(&hol,        (double)r->txsdu_wt_us);
                agg_add(&rx_hol,     (double)r->rxsdu_wt_us);
                agg_add(&retx_depth, (double)r->retransmit_list_depth);
                agg_add(&stall,      (double)r->tx_window_stall_events);
            }
        }
        EXPAND_AGG3(rlc_txpdu_bytes, txpdu_b);
        EXPAND_AGG3(rlc_rxpdu_bytes, rxpdu_b);
        EXPAND_AGG3(rlc_txsdu_bytes, txsdu_b);
        EXPAND_AGG3(rlc_rxsdu_bytes, rxsdu_b);
        EXPAND_AGG3(rlc_lost_pkts,   lost);
        EXPAND_AGG3(rlc_hol_us,      hol);
        EXPAND_AGG3(rlc_rx_hol_us,   rx_hol);
        EXPAND_AGG3(rlc_retx_depth,  retx_depth);
        EXPAND_AGG3(rlc_stall_events, stall);
    }

    /* --- Group 14: Per-UE PDCP → aggregate across active bearers --- */
    {
        agg_t txpdu_b={0}, rxpdu_b={0}, txsdu_b={0}, rxsdu_b={0}, lost={0}, tx_lat={0};

        for (int i = 0; i < MAX_NO_UEs; i++) {
            for (int rb = 0; rb < MAX_RBS_PER_UE; rb++) {
                const aurora_pdcp_radio_bearer_stats_t *p = &m->pdcp_rb_stats[i][rb];
                if (p->rnti == 0) continue;
                s->pdcp_active_bearers++;
                agg_add(&txpdu_b, (double)p->txpdu_bytes);
                agg_add(&rxpdu_b, (double)p->rxpdu_bytes);
                agg_add(&txsdu_b, (double)p->txsdu_bytes);
                agg_add(&rxsdu_b, (double)p->rxsdu_bytes);
                agg_add(&lost,    (double)p->rxpdu_lost_pkts);
                agg_add(&tx_lat,  (double)p->tx_proc_latency_us);
            }
        }
        EXPAND_AGG3(pdcp_txpdu_bytes, txpdu_b);
        EXPAND_AGG3(pdcp_rxpdu_bytes, rxpdu_b);
        EXPAND_AGG3(pdcp_txsdu_bytes, txsdu_b);
        EXPAND_AGG3(pdcp_rxsdu_bytes, rxsdu_b);
        EXPAND_AGG3(pdcp_lost_pkts,   lost);
        EXPAND_AGG3(pdcp_tx_proc_latency_us, tx_lat);
    }

    /* --- Group 15: Per-UE GTP → aggregate across active tunnels --- */
    {
        agg_t tx_b={0}, rx_b={0}, tx_err={0}, rx_err={0}, tx_lat={0};

        for (int i = 0; i < MAX_NO_UEs; i++) {
            for (int t = 0; t < MAX_RBS_PER_UE; t++) {
                const aurora_gtp_tunnel_stats_t *g = &m->gtp_tunnel_stats[i][t];
                if (g->rnti == 0) continue;
                s->gtp_active_tunnels++;
                agg_add(&tx_b,   (double)g->tx_bytes);
                agg_add(&rx_b,   (double)g->rx_bytes);
                agg_add(&tx_err, (double)g->tx_err_pkts);
                agg_add(&rx_err, (double)g->rx_err_pkts);
                agg_add(&tx_lat, (double)g->tx_proc_latency_us);
            }
        }
        EXPAND_AGG3(gtp_tx_bytes, tx_b);
        EXPAND_AGG3(gtp_rx_bytes, rx_b);
        EXPAND_AGG3(gtp_tx_err,   tx_err);
        EXPAND_AGG3(gtp_rx_err,   rx_err);
        EXPAND_AGG3(gtp_tx_proc_latency_us, tx_lat);
    }

#undef EXPAND_AGG3

    /* --- Group 11: RC cell (cell 0 = primary) --- */
    s->rc_cell_count = (int)m->rc_cell_count;
    if (m->rc_cell_count > 0) {
        const aurora_rc_stats_t *rc = &m->rc_metrics[0];
        s->rc0_rsrp_avg        = rc->rsrp_avg;
        s->rc0_sinr_avg        = rc->sinr_avg;
        s->rc0_cell_load_pct   = rc->cell_load_percent;
        s->rc0_interference_lvl = rc->interference_level;
        s->rc0_ho_attempts     = rc->handover_attempts;
        s->rc0_ho_success      = rc->handover_success;
        s->rc0_ho_success_rate = rc->handover_success_rate;
    }

    /* --- UE churn --- */
    s->ue_attach_count = m->ue_attach_count;
    s->ue_detach_count = m->ue_detach_count;

    /* --- CU-side DU connection count --- */
    s->du_count = m->du_count;

    /* --- Group 16: process resource usage --- */
    s->ru_utime_us  = m->ru_utime_us;
    s->ru_stime_us  = m->ru_stime_us;
    s->ru_maxrss_kb = m->ru_maxrss_kb;
    s->ru_minflt    = m->ru_minflt;
    s->ru_majflt    = m->ru_majflt;
    s->ru_inblock   = m->ru_inblock;
    s->ru_oublock   = m->ru_oublock;
    s->ru_nvcsw     = m->ru_nvcsw;
    s->ru_nivcsw    = m->ru_nivcsw;

    /* --- Group 17: Per-thread runtime metrics (mapped by layout) --- */
    for (int i = 0; i < g_thread_ncols; i++) {
        int slot = g_thread_cols[i].slot_idx;
        const aurora_thread_metrics_t *tm = &m->thread_stats.threads[slot];
        thread_snap_t *ts = &s->thread_snaps[i];
        if (tm->valid &&
            strncmp(tm->thread_name, g_thread_cols[i].name, AURORA_THREAD_NAME_LEN) == 0) {
            ts->total_events      = tm->total_events;
            ts->max_runtime_ns    = tm->max_runtime_ns;
            ts->total_runtime_ns  = tm->total_runtime_ns;
            ts->mean_ns           = tm->mean_ns;
            ts->range_ns          = tm->range_ns;
            ts->std_deviation_ns  = tm->std_deviation_ns;
            ts->skewness          = tm->skewness;
            ts->kurtosis          = tm->kurtosis;
            ts->outliers_low      = tm->outliers_low;
            ts->outliers_high     = tm->outliers_high;
            ts->irq_count         = tm->irq_count;
            ts->voluntary_ctxsw   = tm->voluntary_ctxsw;
            ts->involuntary_ctxsw = tm->involuntary_ctxsw;
            ts->cpu_migrations    = tm->cpu_migrations;
            ts->cpu_id            = tm->cpu_id;
        } else {
            memset(ts, 0, sizeof(*ts));
        }
    }
}

/* =========================================================
 * CSV header and row writers
 * ========================================================= */

/* =========================================================
 * Per-role column gating.
 *
 * The fixed CSV schema (one column set for every ran_type) means every
 * role's CSV carries columns it structurally can never populate -- e.g.
 * a CU (no MAC scheduler) always reports zero for every HARQ/CRC/FAPI
 * column. That's harmless (correctly zero, not wrong) but adds ~700
 * always-zero columns per role. Gate column groups by which roles can
 * ever produce real data for them, verified against the actual producer
 * guard clauses in aurora.c/aurora_rlc.c/aurora_pdcp.c and the call-site
 * files for functions with no guard of their own (aurora_gtp.c's guard
 * is permissive/degenerate -- it accepts all 5 valid ran_types -- so GTP
 * is gated by its real call site, nr_pdcp_timer_thread.c, which per
 * AURORA.md only exists on GNB/CU/CU-UP builds).
 *
 * write_csv_header() and write_csv_row() MUST derive gates from this same
 * function -- never duplicate the boolean logic -- so a role's column
 * set can never drift out of lockstep between the two.
 * ========================================================= */
typedef struct {
    bool bh_mh_tx;    /* bhtx_*, bhrx_*, mhtx_* (Group 1, CU-side of the split) */
    bool mhrx;        /* mhrx_* (Group 1) -- bh_mh_tx roles plus DU (DU only reads F1, never writes it) */
    bool mac;         /* f1u_rlc/rlc_f1/rlc_mac/mac_rlc/mac_dl_bo (Group 1), HARQ (2), CRC (3),
                        * SINR/CSI/BSR (4), FAPI DL/UL (5,6), per-UE FAPI (7), per-UE MAC (12),
                        * per-UE RLC (13), and the aggregator-derived half of KPM (8) + RC cell
                        * quality (11) -- everything driven by aurora_recompute_ue_aggregates(),
                        * whose own guard is GNB/DU only (aurora.c) */
    bool kpm_pdcp_bitrate; /* KPM items 9/10 (DRB.PdcpSduBitrateDl/Ul) -- computed *inside* the
                        * GNB/DU-gated aggregator from pdcp_rb_stats, which is itself only
                        * populated on non-DU roles -- so in a CU/DU-split deployment these two
                        * columns can never be non-zero on ANY role, only in monolithic RAN_GNB.
                        * Pre-existing upstream gap (aurora.c's aggregator, not this reader);
                        * gated to GNB only here to reflect what the code actually does rather
                        * than what AURORA.md's per-metric table claims. */
    bool pdcp;        /* per-UE PDCP (Group 14) -- aurora_pdcp.c guard: GNB, CU, CU-UP, CU-CP */
    bool gtp;         /* per-UE GTP (Group 15) -- no GTP-U on DU or CU-CP (AURORA.md) */
    bool handover;    /* rc0_ho_* (Group 11) -- aurora_update_rc_handover() has no guard of its
                        * own; gated by its real call site, rrc_gNB_mobility.c (RRC, CU-side) */
    bool rc_cell_count; /* rc_cell_count -- set by either the aggregator (mac) or first handover
                        * event (handover), so applies whenever either does */
} csv_gates_t;

static csv_gates_t compute_csv_gates(ran_type_t rt)
{
    csv_gates_t g;
    g.bh_mh_tx = (rt == RAN_GNB || rt == RAN_CU || rt == RAN_CU_UP);
    g.mhrx     = g.bh_mh_tx || (rt == RAN_DU);
    g.mac      = (rt == RAN_GNB || rt == RAN_DU);
    g.kpm_pdcp_bitrate = (rt == RAN_GNB);
    g.pdcp     = (rt == RAN_GNB || rt == RAN_CU || rt == RAN_CU_UP || rt == RAN_CU_CP);
    g.gtp      = (rt == RAN_GNB || rt == RAN_CU || rt == RAN_CU_UP);
    g.handover = (rt == RAN_GNB || rt == RAN_CU || rt == RAN_CU_CP);
    g.rc_cell_count = g.mac || g.handover;
    return g;
}

static void write_csv_header(FILE *fp, const struct aurora_metrics *m)
{
    /* Build the thread column layout from the current thread_stats snapshot.
     * The layout is fixed for the lifetime of this CSV file: threads that
     * start after this point are not added; dead threads emit zeros. */
    g_thread_ncols = 0;
    {
        uint32_t nth = (uint32_t)atomic_load_explicit(
            (const _Atomic uint32_t *)&m->thread_stats.num_threads,
            memory_order_relaxed);
        for (uint32_t i = 0; i < nth && g_thread_ncols < AURORA_MAX_MONITORED_THREADS; i++) {
            const aurora_thread_metrics_t *tm = &m->thread_stats.threads[i];
            if (!tm->valid) continue;
            thread_csv_col_t *col = &g_thread_cols[g_thread_ncols++];
            col->slot_idx = (int)i;
            strncpy(col->name, tm->thread_name, AURORA_THREAD_NAME_LEN - 1);
            col->name[AURORA_THREAD_NAME_LEN - 1] = '\0';
            sanitise_thread_name(col->name, col->safe_name, AURORA_THREAD_NAME_LEN);
        }
    }

    /* One column per metric; timestamp is the index. Every subsequent
     * fprintf leads with its own comma so blocks can be gated with plain
     * `if (g.flag) { ... }` -- see compute_csv_gates() above. */
    csv_gates_t g = compute_csv_gates(m->ran_type);
    fprintf(fp, "timestamp_us");

    /* Group 1 — traffic flows (bytes/pkts/mean + Procedure-1 stats) */
#define FLOW_STAT_COLS(f) \
        "," f "_min," f "_max," f "_range," f "_iqr," \
        f "_variance," f "_std_dev," f "_skewness," f "_kurtosis," f "_outliers"
    if (g.bh_mh_tx) {
        fprintf(fp,
            ",bhtx_in_bytes,bhtx_in_pkts,bhtx_in_pkt_mean" FLOW_STAT_COLS("bhtx_in")
            ",bhtx_out_bytes,bhtx_out_pkts,bhtx_out_pkt_mean" FLOW_STAT_COLS("bhtx_out")
            ",bhrx_in_bytes,bhrx_in_pkts,bhrx_in_pkt_mean" FLOW_STAT_COLS("bhrx_in")
            ",bhrx_out_bytes,bhrx_out_pkts,bhrx_out_pkt_mean" FLOW_STAT_COLS("bhrx_out")
            ",mhtx_in_bytes,mhtx_in_pkts,mhtx_in_pkt_mean" FLOW_STAT_COLS("mhtx_in")
            ",mhtx_out_bytes,mhtx_out_pkts,mhtx_out_pkt_mean" FLOW_STAT_COLS("mhtx_out"));
    }
    if (g.mhrx) {
        fprintf(fp,
            ",mhrx_in_bytes,mhrx_in_pkts,mhrx_in_pkt_mean" FLOW_STAT_COLS("mhrx_in")
            ",mhrx_out_bytes,mhrx_out_pkts,mhrx_out_pkt_mean" FLOW_STAT_COLS("mhrx_out"));
    }
    if (g.mac) {
        fprintf(fp,
            ",f1u_rlc_bytes,f1u_rlc_pkts,f1u_rlc_pkt_mean" FLOW_STAT_COLS("f1u_rlc")
            ",rlc_f1_bytes,rlc_f1_pkts,rlc_f1_pkt_mean" FLOW_STAT_COLS("rlc_f1")
            ",rlc_mac_bytes,rlc_mac_pkts,rlc_mac_pkt_mean" FLOW_STAT_COLS("rlc_mac")
            ",mac_rlc_bytes,mac_rlc_pkts,mac_rlc_pkt_mean" FLOW_STAT_COLS("mac_rlc")
            ",mac_dl_bo,mac_dl_bo_samples,mac_dl_bo_mean" FLOW_STAT_COLS("mac_dl_bo"));
    }
#undef FLOW_STAT_COLS

    if (g.mac) {
        fprintf(fp,
            /* Group 2 — HARQ */
            ",dl_harq_ack,dl_harq_nack,dl_harq_total,dl_harq_loss_rate,dl_harq_max_cons"
            /* Group 3 — CRC */
            ",ul_crc_loss,ul_crc_total,ul_crc_loss_rate"
            /* Group 4 — SINR/CSI/BSR */
            ",sinr_min,sinr_max,sinr_avg"
            ",csi_min,csi_max,csi_avg"
            ",bsr_min,bsr_max,bsr_avg"
            /* Group 5 — FAPI DL */
            ",fapi_dl_pdsch_count,fapi_dl_avg_pdsch"
            ",dl_fapi_mcs_min,dl_fapi_mcs_max,dl_fapi_mcs_avg,dl_fapi_mcs_hist_avg"
            ",dl_fapi_prb_min,dl_fapi_prb_max,dl_fapi_prb_avg,dl_fapi_prb_hist_avg"
            ",dl_fapi_tbs_min,dl_fapi_tbs_max,dl_fapi_tbs_avg"
            /* Group 6 — FAPI UL */
            ",fapi_ul_pusch_count,fapi_ul_avg_pusch"
            ",ul_fapi_mcs_min,ul_fapi_mcs_max,ul_fapi_mcs_avg,ul_fapi_mcs_hist_avg"
            ",ul_fapi_prb_min,ul_fapi_prb_max,ul_fapi_prb_avg,ul_fapi_prb_hist_avg"
            ",ul_fapi_tbs_min,ul_fapi_tbs_max,ul_fapi_tbs_avg"
            ",fapi_ue_count"
            /* Group 7 — per-UE FAPI aggregates */
            ",fapi_active_ues"
            ",fapi_ue_dl_mcs_min,fapi_ue_dl_mcs_max,fapi_ue_dl_mcs_avg"
            ",fapi_ue_dl_prb_min,fapi_ue_dl_prb_max,fapi_ue_dl_prb_avg"
            ",fapi_ue_dl_tbs_min,fapi_ue_dl_tbs_max,fapi_ue_dl_tbs_avg"
            ",fapi_ue_ul_mcs_min,fapi_ue_ul_mcs_max,fapi_ue_ul_mcs_avg"
            ",fapi_ue_ul_prb_min,fapi_ue_ul_prb_max,fapi_ue_ul_prb_avg"
            ",fapi_ue_ul_tbs_min,fapi_ue_ul_tbs_max,fapi_ue_ul_tbs_avg"
            /* Group 8 — KPM items 1-8 (MAC-aggregator-derived) */
            ",kpm_RRC.ConnNumber,kpm_DRB.UEThpDl,kpm_DRB.UEThpUl"
            ",kpm_CARR.PDSCHMCSSched,kpm_CARR.PUSCHMCSSched"
            ",kpm_CARR.PRBUsageDl,kpm_CARR.PRBUsageUl"
            ",kpm_CARR.AverageCQI");
    }
    /* KPM items 9/10 -- see kpm_pdcp_bitrate comment on compute_csv_gates() */
    if (g.kpm_pdcp_bitrate) {
        fprintf(fp, ",kpm_DRB.PdcpSduBitrateDl,kpm_DRB.PdcpSduBitrateUl");
    }
    if (g.mac) {
        fprintf(fp,
            /* Group 8 — KPM items 11-16 (MAC-aggregator-derived) */
            ",kpm_L1M.UL-sinrAvg,kpm_L1M.DL-rsrpAvg"
            ",kpm_DRB.RlcPacketDropRateDl,kpm_RRU.PrbTotDl"
            ",kpm_RRU.PrbTotUl,kpm_L1M.UL-dtxRatio"
            /* Group 12 — per-UE MAC aggregates */
            ",mac_active_ues"
            ",mac_dl_tbs_min,mac_dl_tbs_max,mac_dl_tbs_avg"
            ",mac_ul_tbs_min,mac_ul_tbs_max,mac_ul_tbs_avg"
            ",mac_dl_prb_min,mac_dl_prb_max,mac_dl_prb_avg"
            ",mac_ul_prb_min,mac_ul_prb_max,mac_ul_prb_avg"
            ",mac_dl_mcs_min,mac_dl_mcs_max,mac_dl_mcs_avg"
            ",mac_ul_mcs_min,mac_ul_mcs_max,mac_ul_mcs_avg"
            ",mac_pusch_snr_min,mac_pusch_snr_max,mac_pusch_snr_avg"
            ",mac_pucch_snr_min,mac_pucch_snr_max,mac_pucch_snr_avg"
            ",mac_dl_bler_min,mac_dl_bler_max,mac_dl_bler_avg"
            ",mac_ul_bler_min,mac_ul_bler_max,mac_ul_bler_avg"
            ",mac_cqi_min,mac_cqi_max,mac_cqi_avg"
            ",mac_phr_min,mac_phr_max,mac_phr_avg"
            ",mac_bsr_min,mac_bsr_max,mac_bsr_avg"
            /* Group 13 — per-UE RLC aggregates */
            ",rlc_active_bearers"
            ",rlc_txpdu_bytes_min,rlc_txpdu_bytes_max,rlc_txpdu_bytes_avg"
            ",rlc_rxpdu_bytes_min,rlc_rxpdu_bytes_max,rlc_rxpdu_bytes_avg"
            ",rlc_txsdu_bytes_min,rlc_txsdu_bytes_max,rlc_txsdu_bytes_avg"
            ",rlc_rxsdu_bytes_min,rlc_rxsdu_bytes_max,rlc_rxsdu_bytes_avg"
            ",rlc_lost_pkts_min,rlc_lost_pkts_max,rlc_lost_pkts_avg"
            ",rlc_hol_us_min,rlc_hol_us_max,rlc_hol_us_avg"
            ",rlc_rx_hol_us_min,rlc_rx_hol_us_max,rlc_rx_hol_us_avg"
            ",rlc_retx_depth_min,rlc_retx_depth_max,rlc_retx_depth_avg"
            ",rlc_stall_events_min,rlc_stall_events_max,rlc_stall_events_avg");
    }

    if (g.pdcp) {
        fprintf(fp,
            /* Group 14 — per-UE PDCP aggregates */
            ",pdcp_active_bearers"
            ",pdcp_txpdu_bytes_min,pdcp_txpdu_bytes_max,pdcp_txpdu_bytes_avg"
            ",pdcp_rxpdu_bytes_min,pdcp_rxpdu_bytes_max,pdcp_rxpdu_bytes_avg"
            ",pdcp_txsdu_bytes_min,pdcp_txsdu_bytes_max,pdcp_txsdu_bytes_avg"
            ",pdcp_rxsdu_bytes_min,pdcp_rxsdu_bytes_max,pdcp_rxsdu_bytes_avg"
            ",pdcp_lost_pkts_min,pdcp_lost_pkts_max,pdcp_lost_pkts_avg"
            ",pdcp_tx_proc_latency_us_min,pdcp_tx_proc_latency_us_max,pdcp_tx_proc_latency_us_avg");
    }

    if (g.gtp) {
        fprintf(fp,
            /* Group 15 — per-UE GTP aggregates */
            ",gtp_active_tunnels"
            ",gtp_tx_bytes_min,gtp_tx_bytes_max,gtp_tx_bytes_avg"
            ",gtp_rx_bytes_min,gtp_rx_bytes_max,gtp_rx_bytes_avg"
            ",gtp_tx_err_min,gtp_tx_err_max,gtp_tx_err_avg"
            ",gtp_rx_err_min,gtp_rx_err_max,gtp_rx_err_avg"
            ",gtp_tx_proc_latency_us_min,gtp_tx_proc_latency_us_max,gtp_tx_proc_latency_us_avg");
    }

    /* Group 11 — RC cell 0 */
    if (g.rc_cell_count) fprintf(fp, ",rc_cell_count");
    if (g.mac)           fprintf(fp, ",rc0_rsrp_avg,rc0_sinr_avg,rc0_cell_load_pct,rc0_interference_lvl");
    if (g.handover)      fprintf(fp, ",rc0_ho_attempts,rc0_ho_success,rc0_ho_success_rate");

    /* UE churn -- driven by gNB_scheduler_primitives.c (MAC scheduler, same gate as g.mac) */
    if (g.mac) fprintf(fp, ",ue_attach_count,ue_detach_count");
    /* du_count: CU-side gauge, same gate as rc0_ho_* (aurora_update_du_count()
     * is called from rrc_cell_management.c, same RRC/CU-side layer as
     * aurora_update_rc_handover()). */
    if (g.handover) fprintf(fp, ",du_count");

    /* Group 16 — process resource usage (universal: every role's own process) */
    fprintf(fp,
        ",ru_utime_us,ru_stime_us,ru_maxrss_kb"
        ",ru_minflt,ru_majflt,ru_inblock,ru_oublock,ru_nvcsw,ru_nivcsw");

    /* Group 17 — per-thread runtime metrics (one column group per monitored thread) */
    for (int i = 0; i < g_thread_ncols; i++) {
        const char *n = g_thread_cols[i].safe_name;
        fprintf(fp,
            ",THREAD_%s_total_events"
            ",THREAD_%s_max_runtime_ns"
            ",THREAD_%s_total_runtime_ns"
            ",THREAD_%s_mean_ns"
            ",THREAD_%s_range_ns"
            ",THREAD_%s_std_dev_ns"
            ",THREAD_%s_skewness"
            ",THREAD_%s_kurtosis"
            ",THREAD_%s_outliers_low"
            ",THREAD_%s_outliers_high"
            ",THREAD_%s_irq_count"
            ",THREAD_%s_voluntary_ctxsw"
            ",THREAD_%s_involuntary_ctxsw"
            ",THREAD_%s_cpu_migrations"
            ",THREAD_%s_cpu_id",
            n, n, n, n, n, n, n, n, n, n, n, n, n, n, n);
    }

    /* Group 18 — Infrastructure metrics (container + host + GPU) */
    if (g_infra_metrics) {
        fprintf(fp,
            /* Container CPU */
            ",infra_ctr_cpu_pct,infra_ctr_cpu_usage_us"
            ",infra_ctr_cpu_throttled_us,infra_ctr_cpu_throttle_count"
            ",infra_ctr_vol_ctxsw,infra_ctr_invol_ctxsw"
            /* Container memory */
            ",infra_ctr_mem_bytes,infra_ctr_mem_rss,infra_ctr_mem_cache"
            ",infra_ctr_mem_limit,infra_ctr_mem_pct,infra_ctr_mem_swap"
            ",infra_ctr_mem_oom_kills,infra_ctr_mem_pgfault,infra_ctr_mem_pgmajfault"
            ",infra_ctr_mem_psi_some10,infra_ctr_mem_psi_full10"
            /* Container network */
            ",infra_ctr_net_rx_bytes,infra_ctr_net_tx_bytes"
            ",infra_ctr_net_rx_pkts,infra_ctr_net_tx_pkts"
            ",infra_ctr_net_rx_errs,infra_ctr_net_tx_errs"
            ",infra_ctr_net_carrier_changes"
            ",infra_ctr_tcp_retrans,infra_ctr_tcp_estab"
            /* Container I/O */
            ",infra_ctr_io_read_bytes,infra_ctr_io_write_bytes"
            ",infra_ctr_io_read_ops,infra_ctr_io_write_ops"
            /* Container proc */
            ",infra_ctr_open_fds,infra_ctr_fd_limit"
            ",infra_ctr_threads,infra_ctr_processes"
            /* Host thermal (zone 0) */
            ",infra_host_thermal0_mc,infra_host_thermal0_trip"
            /* Host CPU */
            ",infra_host_load1,infra_host_load5,infra_host_load15"
            ",infra_host_cpu_user_pct,infra_host_cpu_sys_pct"
            ",infra_host_cpu_iowait_pct,infra_host_cpu_idle_pct"
            ",infra_host_cpu_psi_some10"
            ",infra_host_softirq_netrx,infra_host_softirq_nettx"
            /* Host memory */
            ",infra_host_mem_total_kb,infra_host_mem_avail_kb"
            ",infra_host_swap_total_kb,infra_host_swap_free_kb"
            ",infra_host_mem_psi_some10,infra_host_mem_psi_full10"
            /* Host disk */
            ",infra_host_disk_read_ios,infra_host_disk_write_ios"
            ",infra_host_disk_util_pct"
            ",infra_host_io_psi_some10,infra_host_io_psi_full10"
            /* Host network (NIC 0) */
            ",infra_host_nic0_rx_bytes,infra_host_nic0_tx_bytes"
            ",infra_host_nic0_rx_errs,infra_host_nic0_tx_errs"
            ",infra_host_nic0_carrier_changes"
            /* Host kernel */
            ",infra_host_entropy,infra_host_uptime_s"
            /* GPU 0 (if present) */
            ",infra_gpu0_util_pct,infra_gpu0_mem_util_pct"
            ",infra_gpu0_mem_used_mib,infra_gpu0_temp_c"
            ",infra_gpu0_power_mw,infra_gpu0_ecc_sbit,infra_gpu0_ecc_dbit");
    }

    fprintf(fp, "\n");
}

static void write_csv_row(FILE *fp, const snapshot_t *s)
{
    /* Must derive the exact same gates as write_csv_header() for this
     * role, or the row's column count silently drifts from the header's. */
    csv_gates_t g = compute_csv_gates(s->ran_type);

    fprintf(fp, "%lu", s->timestamp_us);

    /* Group 1 (bytes/pkts/mean + Procedure-1 stats per flow) */
#define FLOW_STAT_FMT ",%.2f,%.2f,%.2f,%.2f,%.4f,%.4f,%.4f,%.4f,%u"
#define FLOW_STAT_ARGS(field) \
        s->field##_stats.min, s->field##_stats.max, s->field##_stats.range, s->field##_stats.iqr, \
        s->field##_stats.variance, s->field##_stats.std_dev, s->field##_stats.skewness, \
        s->field##_stats.kurtosis, s->field##_stats.outliers
    if (g.bh_mh_tx) {
        fprintf(fp,
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT,
            s->bhtx_in_bytes,  s->bhtx_in_pkts,  s->bhtx_in_pkt_mean,  FLOW_STAT_ARGS(bhtx_in),
            s->bhtx_out_bytes, s->bhtx_out_pkts, s->bhtx_out_pkt_mean, FLOW_STAT_ARGS(bhtx_out),
            s->bhrx_in_bytes,  s->bhrx_in_pkts,  s->bhrx_in_pkt_mean,  FLOW_STAT_ARGS(bhrx_in),
            s->bhrx_out_bytes, s->bhrx_out_pkts, s->bhrx_out_pkt_mean, FLOW_STAT_ARGS(bhrx_out),
            s->mhtx_in_bytes,  s->mhtx_in_pkts,  s->mhtx_in_pkt_mean,  FLOW_STAT_ARGS(mhtx_in),
            s->mhtx_out_bytes, s->mhtx_out_pkts, s->mhtx_out_pkt_mean, FLOW_STAT_ARGS(mhtx_out));
    }
    if (g.mhrx) {
        fprintf(fp,
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT,
            s->mhrx_in_bytes,  s->mhrx_in_pkts,  s->mhrx_in_pkt_mean,  FLOW_STAT_ARGS(mhrx_in),
            s->mhrx_out_bytes, s->mhrx_out_pkts, s->mhrx_out_pkt_mean, FLOW_STAT_ARGS(mhrx_out));
    }
    if (g.mac) {
        fprintf(fp,
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT
            ",%lu,%u,%.2f" FLOW_STAT_FMT,
            s->f1u_rlc_bytes,  s->f1u_rlc_pkts,  s->f1u_rlc_pkt_mean,  FLOW_STAT_ARGS(f1u_rlc),
            s->rlc_f1_bytes,   s->rlc_f1_pkts,   s->rlc_f1_pkt_mean,   FLOW_STAT_ARGS(rlc_f1),
            s->rlc_mac_bytes,  s->rlc_mac_pkts,  s->rlc_mac_pkt_mean,  FLOW_STAT_ARGS(rlc_mac),
            s->mac_rlc_bytes,  s->mac_rlc_pkts,  s->mac_rlc_pkt_mean,  FLOW_STAT_ARGS(mac_rlc),
            s->mac_dl_bo,      s->mac_dl_bo_smpl, s->mac_dl_bo_mean,   FLOW_STAT_ARGS(mac_dl_bo));
    }
#undef FLOW_STAT_FMT
#undef FLOW_STAT_ARGS

    if (g.mac) {
        fprintf(fp,
            /* Group 2 */
            ",%lu,%lu,%lu,%.6f,%u"
            /* Group 3 */
            ",%lu,%lu,%.6f"
            /* Group 4 */
            ",%.2f,%.2f,%.2f"
            ",%u,%u,%.2f"
            ",%u,%u,%.2f"
            /* Group 5 */
            ",%u,%.2f"
            ",%u,%u,%.2f,%.2f"
            ",%u,%u,%.2f,%.2f"
            ",%u,%u,%.2f"
            /* Group 6 */
            ",%u,%.2f"
            ",%u,%u,%.2f,%.2f"
            ",%u,%u,%.2f,%.2f"
            ",%u,%u,%.2f"
            ",%u"
            /* Group 7 — per-UE FAPI aggregates */
            ",%d"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            /* Group 8 — KPM items 1-8 */
            ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f",

            /* Group 2 */
            s->dl_harq_ack, s->dl_harq_nack, s->dl_harq_total,
            s->dl_harq_loss_rate, s->dl_harq_max_cons,

            /* Group 3 */
            s->ul_crc_loss, s->ul_crc_total, s->ul_crc_loss_rate,

            /* Group 4 */
            (double)s->sinr_min, (double)s->sinr_max, (double)s->sinr_avg,
            (unsigned)s->csi_min, (unsigned)s->csi_max, (double)s->csi_avg,
            s->bsr_min, s->bsr_max, (double)s->bsr_avg,

            /* Group 5 */
            s->fapi_dl_pdsch_count, (double)s->fapi_dl_avg_pdsch,
            (unsigned)s->dl_fapi_mcs_min, (unsigned)s->dl_fapi_mcs_max, (double)s->dl_fapi_mcs_avg, s->dl_fapi_mcs_hist_avg,
            s->dl_fapi_prb_min, s->dl_fapi_prb_max, (double)s->dl_fapi_prb_avg, s->dl_fapi_prb_hist_avg,
            s->dl_fapi_tbs_min, s->dl_fapi_tbs_max, (double)s->dl_fapi_tbs_avg,

            /* Group 6 */
            s->fapi_ul_pusch_count, (double)s->fapi_ul_avg_pusch,
            (unsigned)s->ul_fapi_mcs_min, (unsigned)s->ul_fapi_mcs_max, (double)s->ul_fapi_mcs_avg, s->ul_fapi_mcs_hist_avg,
            s->ul_fapi_prb_min, s->ul_fapi_prb_max, (double)s->ul_fapi_prb_avg, s->ul_fapi_prb_hist_avg,
            s->ul_fapi_tbs_min, s->ul_fapi_tbs_max, (double)s->ul_fapi_tbs_avg,
            s->fapi_ue_count,

            /* Group 7 — per-UE FAPI aggregates */
            s->fapi_active_ues,
            s->fapi_ue_dl_mcs_min, s->fapi_ue_dl_mcs_max, s->fapi_ue_dl_mcs_avg,
            s->fapi_ue_dl_prb_min, s->fapi_ue_dl_prb_max, s->fapi_ue_dl_prb_avg,
            s->fapi_ue_dl_tbs_min, s->fapi_ue_dl_tbs_max, s->fapi_ue_dl_tbs_avg,
            s->fapi_ue_ul_mcs_min, s->fapi_ue_ul_mcs_max, s->fapi_ue_ul_mcs_avg,
            s->fapi_ue_ul_prb_min, s->fapi_ue_ul_prb_max, s->fapi_ue_ul_prb_avg,
            s->fapi_ue_ul_tbs_min, s->fapi_ue_ul_tbs_max, s->fapi_ue_ul_tbs_avg,

            /* Group 8 — KPM items 1-8 */
            s->kpm[0], s->kpm[1], s->kpm[2], s->kpm[3],
            s->kpm[4], s->kpm[5], s->kpm[6], s->kpm[7]);
    }

    /* KPM items 9/10 -- see kpm_pdcp_bitrate comment on compute_csv_gates() */
    if (g.kpm_pdcp_bitrate) {
        fprintf(fp, ",%.2f,%.2f", s->kpm[8], s->kpm[9]);
    }

    if (g.mac) {
        fprintf(fp,
            /* Group 8 — KPM items 11-16 */
            ",%.2f,%.2f,%.6f,%.2f,%.2f,%.6f"
            /* Group 12 — MAC aggregates */
            ",%d"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.4f,%.4f,%.4f"
            ",%.4f,%.4f,%.4f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            /* Group 13 — RLC aggregates */
            ",%d"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f",

            /* Group 8 — KPM items 11-16 */
            s->kpm[10], s->kpm[11], s->kpm[12], s->kpm[13], s->kpm[14], s->kpm[15],

            /* Group 12 — MAC */
            s->mac_active_ues,
            s->mac_dl_tbs_min,    s->mac_dl_tbs_max,    s->mac_dl_tbs_avg,
            s->mac_ul_tbs_min,    s->mac_ul_tbs_max,    s->mac_ul_tbs_avg,
            s->mac_dl_prb_min,    s->mac_dl_prb_max,    s->mac_dl_prb_avg,
            s->mac_ul_prb_min,    s->mac_ul_prb_max,    s->mac_ul_prb_avg,
            s->mac_dl_mcs_min,    s->mac_dl_mcs_max,    s->mac_dl_mcs_avg,
            s->mac_ul_mcs_min,    s->mac_ul_mcs_max,    s->mac_ul_mcs_avg,
            s->mac_pusch_snr_min, s->mac_pusch_snr_max, s->mac_pusch_snr_avg,
            s->mac_pucch_snr_min, s->mac_pucch_snr_max, s->mac_pucch_snr_avg,
            s->mac_dl_bler_min,   s->mac_dl_bler_max,   s->mac_dl_bler_avg,
            s->mac_ul_bler_min,   s->mac_ul_bler_max,   s->mac_ul_bler_avg,
            s->mac_cqi_min,       s->mac_cqi_max,       s->mac_cqi_avg,
            s->mac_phr_min,       s->mac_phr_max,       s->mac_phr_avg,
            s->mac_bsr_min,       s->mac_bsr_max,       s->mac_bsr_avg,

            /* Group 13 — RLC */
            s->rlc_active_bearers,
            s->rlc_txpdu_bytes_min, s->rlc_txpdu_bytes_max, s->rlc_txpdu_bytes_avg,
            s->rlc_rxpdu_bytes_min, s->rlc_rxpdu_bytes_max, s->rlc_rxpdu_bytes_avg,
            s->rlc_txsdu_bytes_min, s->rlc_txsdu_bytes_max, s->rlc_txsdu_bytes_avg,
            s->rlc_rxsdu_bytes_min, s->rlc_rxsdu_bytes_max, s->rlc_rxsdu_bytes_avg,
            s->rlc_lost_pkts_min,   s->rlc_lost_pkts_max,   s->rlc_lost_pkts_avg,
            s->rlc_hol_us_min,      s->rlc_hol_us_max,      s->rlc_hol_us_avg,
            s->rlc_rx_hol_us_min,   s->rlc_rx_hol_us_max,   s->rlc_rx_hol_us_avg,
            s->rlc_retx_depth_min,  s->rlc_retx_depth_max,  s->rlc_retx_depth_avg,
            s->rlc_stall_events_min, s->rlc_stall_events_max, s->rlc_stall_events_avg);
    }

    if (g.pdcp) {
        fprintf(fp,
            /* Group 14 — PDCP */
            ",%d"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f",
            s->pdcp_active_bearers,
            s->pdcp_txpdu_bytes_min, s->pdcp_txpdu_bytes_max, s->pdcp_txpdu_bytes_avg,
            s->pdcp_rxpdu_bytes_min, s->pdcp_rxpdu_bytes_max, s->pdcp_rxpdu_bytes_avg,
            s->pdcp_txsdu_bytes_min, s->pdcp_txsdu_bytes_max, s->pdcp_txsdu_bytes_avg,
            s->pdcp_rxsdu_bytes_min, s->pdcp_rxsdu_bytes_max, s->pdcp_rxsdu_bytes_avg,
            s->pdcp_lost_pkts_min,   s->pdcp_lost_pkts_max,   s->pdcp_lost_pkts_avg,
            s->pdcp_tx_proc_latency_us_min, s->pdcp_tx_proc_latency_us_max, s->pdcp_tx_proc_latency_us_avg);
    }

    if (g.gtp) {
        fprintf(fp,
            /* Group 15 — GTP */
            ",%d"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f"
            ",%.2f,%.2f,%.2f",
            s->gtp_active_tunnels,
            s->gtp_tx_bytes_min, s->gtp_tx_bytes_max, s->gtp_tx_bytes_avg,
            s->gtp_rx_bytes_min, s->gtp_rx_bytes_max, s->gtp_rx_bytes_avg,
            s->gtp_tx_err_min,   s->gtp_tx_err_max,   s->gtp_tx_err_avg,
            s->gtp_rx_err_min,   s->gtp_rx_err_max,   s->gtp_rx_err_avg,
            s->gtp_tx_proc_latency_us_min, s->gtp_tx_proc_latency_us_max, s->gtp_tx_proc_latency_us_avg);
    }

    /* Group 11 — RC cell 0 */
    if (g.rc_cell_count) fprintf(fp, ",%d", s->rc_cell_count);
    if (g.mac) {
        fprintf(fp, ",%.2f,%.2f,%u,%u",
            s->rc0_rsrp_avg, s->rc0_sinr_avg,
            s->rc0_cell_load_pct, s->rc0_interference_lvl);
    }
    if (g.handover) {
        fprintf(fp, ",%u,%u,%.4f",
            s->rc0_ho_attempts, s->rc0_ho_success, s->rc0_ho_success_rate);
    }

    /* UE churn -- same gate as g.mac, see write_csv_header() */
    if (g.mac) {
        fprintf(fp, ",%u,%u", s->ue_attach_count, s->ue_detach_count);
    }
    /* du_count -- same gate as g.handover, see write_csv_header() */
    if (g.handover) {
        fprintf(fp, ",%u", s->du_count);
    }

    /* Group 16 — rusage (universal) */
    fprintf(fp,
        ",%lu,%lu,%lu"
        ",%lu,%lu,%lu,%lu,%lu,%lu",
        s->ru_utime_us, s->ru_stime_us, s->ru_maxrss_kb,
        s->ru_minflt, s->ru_majflt, s->ru_inblock,
        s->ru_oublock, s->ru_nvcsw, s->ru_nivcsw);

    /* Group 17 — per-thread runtime metrics */
    for (int i = 0; i < g_thread_ncols; i++) {
        const thread_snap_t *t = &s->thread_snaps[i];
        fprintf(fp,
            ",%lu,%lu,%.3f,%.3f,%.3f,%.3f,%.6f,%.6f,%lu,%lu,%lu,%lu,%lu,%lu,%d",
            t->total_events, t->max_runtime_ns,
            t->total_runtime_ns, t->mean_ns, t->range_ns, t->std_deviation_ns,
            t->skewness, t->kurtosis,
            t->outliers_low, t->outliers_high,
            t->irq_count, t->voluntary_ctxsw, t->involuntary_ctxsw,
            t->cpu_migrations, t->cpu_id);
    }

    /* Group 18 — Infrastructure metrics */
    if (g_infra_metrics) {
        const aurora_infra_metrics_t *im = g_infra_metrics;
        const aurora_container_metrics_t *c = &im->container;
        const aurora_host_metrics_t *h = &im->host;

        fprintf(fp,
            /* Container CPU */
            ",%.2f,%lu,%lu,%u,%lu,%lu"
            /* Container memory */
            ",%lu,%lu,%lu,%lu,%.2f,%lu,%u,%lu,%lu,%.2f,%.2f"
            /* Container network */
            ",%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu"
            /* Container I/O */
            ",%lu,%lu,%lu,%lu"
            /* Container proc */
            ",%u,%u,%u,%u"
            /* Host thermal zone 0 */
            ",%d,%d"
            /* Host CPU */
            ",%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lu,%lu"
            /* Host memory */
            ",%lu,%lu,%lu,%lu,%.2f,%.2f"
            /* Host disk */
            ",%lu,%lu,%.2f,%.2f,%.2f"
            /* Host network NIC 0 */
            ",%lu,%lu,%lu,%lu,%lu"
            /* Host kernel */
            ",%u,%.1f"
            /* GPU 0 */
            ",%u,%u,%lu,%d,%u,%lu,%lu",

            /* Container CPU values */
            c->cpu.cpu_usage_pct, (unsigned long)c->cpu.cpu_usage_us,
            (unsigned long)c->cpu.cpu_throttled_us, c->cpu.cpu_throttle_count,
            (unsigned long)c->cpu.voluntary_ctxsw, (unsigned long)c->cpu.involuntary_ctxsw,

            /* Container memory values */
            (unsigned long)c->mem.mem_current_bytes, (unsigned long)c->mem.mem_rss_bytes,
            (unsigned long)c->mem.mem_cache_bytes, (unsigned long)c->mem.mem_limit_bytes,
            c->mem.mem_usage_pct, (unsigned long)c->mem.mem_swap_bytes,
            c->mem.mem_oom_kill_count,
            (unsigned long)c->mem.mem_pgfault, (unsigned long)c->mem.mem_pgmajfault,
            (double)c->mem.mem_psi_some_avg10, (double)c->mem.mem_psi_full_avg10,

            /* Container network values */
            (unsigned long)c->net.rx_bytes, (unsigned long)c->net.tx_bytes,
            (unsigned long)c->net.rx_packets, (unsigned long)c->net.tx_packets,
            (unsigned long)c->net.rx_errors, (unsigned long)c->net.tx_errors,
            (unsigned long)c->net.carrier_changes,
            (unsigned long)c->net.tcp_retransmits, (unsigned long)c->net.tcp_curr_estab,

            /* Container I/O values */
            (unsigned long)c->io.io_read_bytes, (unsigned long)c->io.io_write_bytes,
            (unsigned long)c->io.io_read_ops, (unsigned long)c->io.io_write_ops,

            /* Container proc values */
            c->proc.open_fds, c->proc.fd_limit,
            c->proc.thread_count, c->proc.process_count,

            /* Host thermal zone 0 */
            (h->thermal.num_zones > 0) ? h->thermal.zones[0].temp_millicelsius : 0,
            (h->thermal.num_zones > 0) ? h->thermal.zones[0].trip_point_0_mc : 0,

            /* Host CPU */
            (double)h->cpu.load_1min, (double)h->cpu.load_5min, (double)h->cpu.load_15min,
            h->cpu.cpu_user_pct, h->cpu.cpu_system_pct,
            h->cpu.cpu_iowait_pct, h->cpu.cpu_idle_pct,
            (double)h->cpu.cpu_psi_some_avg10,
            (unsigned long)h->cpu.softirq_net_rx, (unsigned long)h->cpu.softirq_net_tx,

            /* Host memory */
            (unsigned long)h->mem.mem_total_kb, (unsigned long)h->mem.mem_available_kb,
            (unsigned long)h->mem.swap_total_kb, (unsigned long)h->mem.swap_free_kb,
            (double)h->mem.mem_psi_some_avg10, (double)h->mem.mem_psi_full_avg10,

            /* Host disk */
            (unsigned long)h->disk.read_ios, (unsigned long)h->disk.write_ios,
            h->disk.disk_util_pct,
            (double)h->disk.io_psi_some_avg10, (double)h->disk.io_psi_full_avg10,

            /* Host network NIC 0 */
            (h->net.num_nics > 0) ? (unsigned long)h->net.nics[0].rx_bytes : 0UL,
            (h->net.num_nics > 0) ? (unsigned long)h->net.nics[0].tx_bytes : 0UL,
            (h->net.num_nics > 0) ? (unsigned long)h->net.nics[0].rx_errors : 0UL,
            (h->net.num_nics > 0) ? (unsigned long)h->net.nics[0].tx_errors : 0UL,
            (h->net.num_nics > 0) ? (unsigned long)h->net.nics[0].carrier_changes : 0UL,

            /* Host kernel */
            h->kernel.entropy_avail, h->kernel.uptime_sec,

            /* GPU 0 */
            (im->gpu.num_gpus > 0) ? im->gpu.gpus[0].gpu_util_pct : 0,
            (im->gpu.num_gpus > 0) ? im->gpu.gpus[0].mem_util_pct : 0,
            (im->gpu.num_gpus > 0) ? (unsigned long)im->gpu.gpus[0].mem_used_mib : 0UL,
            (im->gpu.num_gpus > 0) ? im->gpu.gpus[0].temperature_c : 0,
            (im->gpu.num_gpus > 0) ? im->gpu.gpus[0].power_draw_mw : 0,
            (im->gpu.num_gpus > 0) ? (unsigned long)im->gpu.gpus[0].ecc_single_bit : 0UL,
            (im->gpu.num_gpus > 0) ? (unsigned long)im->gpu.gpus[0].ecc_double_bit : 0UL);
    }

    fprintf(fp, "\n");
}

/* =========================================================
 * Signal handling for graceful shutdown
 * ========================================================= */

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

/* =========================================================
 * main
 * ========================================================= */

#define DEFAULT_BATCH_SIZE 100

/* =========================================================
 * Minimal reader of the OCUDU YAML "aurora:" section (no YAML library dependency):
 *   aurora:
 *     shm_key: 0xDEADBEEF
 *     collection_interval_ms: 100
 *     ran_type: du        # auto|gnb|du|cu|cu_cp|cu_up
 * ========================================================= */
static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    if ((*s == '"' || *s == '\'') && e > s + 1 && e[-1] == *s) { e[-1] = '\0'; s++; }
    return s;
}

static int load_aurora_yaml_section(const char *path, aurora_config_t *cfg, bool keep_ran_type)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char line[1024];
    int in_section = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        if (line[0] != ' ' && line[0] != '\t' && line[0] != '\n' && line[0] != '\0') {
            in_section = (strncmp(line, "aurora:", 7) == 0);
            continue;
        }
        if (!in_section) continue;
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *key = trim(line);
        char *val = trim(colon + 1);
        if (!strcmp(key, "shm_key")) {
            cfg->shm_key = (key_t)strtoul(val, NULL, 0);
        } else if (!strcmp(key, "collection_interval_ms")) {
            cfg->collection_interval_ms = (uint32_t)strtoul(val, NULL, 0);
        } else if (!strcmp(key, "ran_type") && !keep_ran_type) {
            if      (!strcmp(val, "gnb"))   cfg->ran_type = RAN_GNB;
            else if (!strcmp(val, "du"))    cfg->ran_type = RAN_DU;
            else if (!strcmp(val, "cu"))    cfg->ran_type = RAN_CU;
            else if (!strcmp(val, "cu_cp")) cfg->ran_type = RAN_CU_CP;
            else if (!strcmp(val, "cu_up")) cfg->ran_type = RAN_CU_UP;
        }
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    /* --- argument parsing --- */
    aurora_config_t aurora_cfg = {0};
    aurora_cfg.collection_interval_ms = 100;
    aurora_cfg.shm_key  = 0xDEADBEEF;
    aurora_cfg.ran_type = RAN_GNB;

    char csv_path[512]    = "";
    bool print_once       = false;
    int  batch_size       = DEFAULT_BATCH_SIZE;
    char *config_file     = NULL;
    bool ran_type_from_cli = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "Usage: %s [config_file] [options]\n\n"
                "Continuous collection mode (default):\n"
                "  Polls shared memory every <interval> ms.\n"
                "  After every <samples> samples, appends rows to the CSV.\n"
                "  Each row = one collection interval; timestamp_us is the index.\n"
                "  Per-UE metrics are aggregated across active UEs/bearers/tunnels\n"
                "  into _min, _max, _avg columns.\n\n"
                "Options:\n"
                "  --ran-type <type>    RAN_GNB|RAN_DU|RAN_CU|RAN_CU_CP|RAN_CU_UP\n"
                "  --shm-key <hex>      Shared memory key (default 0xDEADBEEF)\n"
                "  --interval <ms>      Poll interval in ms (default 100)\n"
                "  --output <file>      CSV output file (default: aurora_metrics_<ts>.csv)\n"
                "  --samples <N>        Rows to buffer before flushing to CSV (default %d)\n"
                "  --once               Print one formatted snapshot and exit\n"
                "\nInfrastructure metrics (sidecar mode):\n"
                "  --infra-enabled yes|no     Enable infra metrics collection\n"
                "  --infra-shm-key <hex>      Infra SHM key (default 0xAEF10001)\n"
                "  --infra-interval <ms>      Default infra poll interval (default 1000)\n"
                "  --infra-container <name>   Target container name\n"
                "  --infra-discover <mode>    manual|auto (default auto)\n"
                "  --infra-gpu yes|no         Enable GPU metrics\n"
                "  --infra-host-proc <prefix> Host /proc mount prefix (default /host)\n"
                "  --infra-host-sys <prefix>  Host /sys mount prefix (default /host)\n"
                "  --infra-cpu-interval <ms>  CPU poll interval (0=use default)\n"
                "  --infra-mem-interval <ms>  Memory poll interval\n"
                "  --infra-net-interval <ms>  Network poll interval\n"
                "  --infra-disk-interval <ms> Disk poll interval\n"
                "  --infra-host-interval <ms> Host metrics poll interval\n"
                "  --infra-gpu-interval <ms>  GPU poll interval\n"
                "\n  --help, -h           Show this help\n",
                argv[0], DEFAULT_BATCH_SIZE);
            return 0;
        } else if (!strcmp(argv[i], "--ran-type") && i + 1 < argc) {
            i++;
            ran_type_from_cli = true;
            if      (!strcmp(argv[i], "RAN_GNB")   || !strcmp(argv[i], "gnb"))   aurora_cfg.ran_type = RAN_GNB;
            else if (!strcmp(argv[i], "RAN_DU")    || !strcmp(argv[i], "du"))    aurora_cfg.ran_type = RAN_DU;
            else if (!strcmp(argv[i], "RAN_CU")    || !strcmp(argv[i], "cu"))    aurora_cfg.ran_type = RAN_CU;
            else if (!strcmp(argv[i], "RAN_CU_CP") || !strcmp(argv[i], "cu-cp") || !strcmp(argv[i], "cu_cp")) aurora_cfg.ran_type = RAN_CU_CP;
            else if (!strcmp(argv[i], "RAN_CU_UP") || !strcmp(argv[i], "cu-up") || !strcmp(argv[i], "cu_up")) aurora_cfg.ran_type = RAN_CU_UP;
        } else if (!strcmp(argv[i], "--shm-key") && i + 1 < argc) {
            aurora_cfg.shm_key = (key_t)strtol(argv[++i], NULL, 16);
        } else if (!strcmp(argv[i], "--interval") && i + 1 < argc) {
            aurora_cfg.collection_interval_ms = (uint32_t)atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--output") && i + 1 < argc) {
            snprintf(csv_path, sizeof(csv_path), "%s", argv[++i]);
        } else if (!strcmp(argv[i], "--samples") && i + 1 < argc) {
            batch_size = atoi(argv[++i]);
            if (batch_size < 1) batch_size = 1;
        } else if (!strcmp(argv[i], "--once")) {
            print_once = true;
        } else if (!strncmp(argv[i], "--infra-", 8)) {
            /* Parsed later by aurora_infra_parse_cli(); skip the flag and its value. */
            if (i + 1 < argc) i++;
        } else if (argv[i][0] != '-' && !config_file) {
            config_file = argv[i];
        } else {
            fprintf(stderr, "aurora_reader: ignoring unknown option '%s'\n", argv[i]);
        }
    }

    /* OCUDU: read shm_key / collection_interval_ms / ran_type from the "aurora:" section of the
     * application YAML config file, if provided. CLI options given explicitly still win. */
    if (config_file) {
        if (load_aurora_yaml_section(config_file, &aurora_cfg, ran_type_from_cli) != 0)
            fprintf(stderr, "Warning: failed to load aurora section from %s\n", config_file);
    }

    /* Attach to shared memory */
    struct aurora_metrics *metrics = aurora_get_metrics_by_key(aurora_cfg.shm_key);

    if (!metrics) {
        fprintf(stderr, "aurora_reader: cannot attach to shared memory key 0x%x "
                        "(is the OCUDU app running with aurora.enable=true?)\n",
                aurora_cfg.shm_key);
        return 1;
    }

    /* OCUDU: the producing process publishes its role in shared memory; use it unless forced on the CLI. */
    if (!ran_type_from_cli && metrics->ran_type <= RAN_CU_UP)
        aurora_cfg.ran_type = metrics->ran_type;

    /* Infrastructure metrics — CLI-only configuration.
     * Config file is NOT consulted for the infra section so the reader
     * is fully controlled by command-line arguments. */
    g_infra_cfg = aurora_infra_default_config();
    aurora_infra_parse_cli(&g_infra_cfg, argc, argv);

    if (g_infra_cfg.enabled) {
        /* Auto-derive unique SHM key from ran_type if CLI did not override.
         * Default is 0xAEF10001; readers for different ran_types collide
         * on the same key otherwise. */
        {
            static const key_t default_key = (key_t)0xAEF10001;
            if (g_infra_cfg.infra_shm_key == default_key) {
                static const key_t per_ran[] = {
                    0xAEF10000,  /* RAN_GNB  */
                    0xAEF10001,  /* RAN_DU   */
                    0xAEF10002,  /* RAN_CU   */
                    0xAEF10003,  /* RAN_CU_CP */
                    0xAEF10004,  /* RAN_CU_UP */
                };
                if (aurora_cfg.ran_type <= RAN_CU_UP)
                    g_infra_cfg.infra_shm_key = per_ran[aurora_cfg.ran_type];
            }
        }

        /* Auto-select container from ran_type if not provided via CLI. */
        if (g_infra_cfg.container_name[0] == '\0') {
            static const char *ran_hint[] = {"gnb","du","cu","cu_cp","cu_up"};
            const char *hint = (aurora_cfg.ran_type <= RAN_CU_UP)
                               ? ran_hint[aurora_cfg.ran_type] : "gnb";
            aurora_infra_auto_select_container(&g_infra_cfg, hint);
        }

        fprintf(stderr,
                "aurora_reader: infra cfg: container='%s' shm_key=0x%x "
                "host_proc='%s' host_sys='%s'\n",
                g_infra_cfg.container_name, g_infra_cfg.infra_shm_key,
                g_infra_cfg.host_proc_prefix, g_infra_cfg.host_sys_prefix);

        if (aurora_infra_init(&g_infra_cfg) == 0) {
            /* Attach read-only view for CSV export */
            g_infra_metrics = aurora_infra_get_metrics(g_infra_cfg.infra_shm_key);
            if (!g_infra_metrics)
                fprintf(stderr, "aurora_reader: infra SHM attach failed (continuing without)\n");
            else
                fprintf(stderr, "aurora_reader: infra metrics enabled (key=0x%x)\n",
                        g_infra_cfg.infra_shm_key);
        }
    }

    /* -----------------------------------------------------------
     * --once mode: print a single formatted snapshot and exit.
     * ----------------------------------------------------------- */
    if (print_once) {
        static const char *ran_names[] = {"RAN_GNB","RAN_DU","RAN_CU","RAN_CU_CP","RAN_CU_UP"};
        const char *rn = (aurora_cfg.ran_type <= RAN_CU_UP) ? ran_names[aurora_cfg.ran_type] : "?";
        printf("AURORA snapshot  ran_type=%s  key=0x%x  ts=%lu us\n",
               rn, aurora_cfg.shm_key, metrics->timestamp_us);

        snapshot_t s;
        fill_snapshot(&s, metrics);

        printf("\n--- Traffic flows (bytes | pkts | mean-pkt-bytes) ---\n");
#define PRINT_FLOW(name, field) \
        printf("  %-12s  %12lu B  %8u pkts  mean=%8.1f B\n", \
               name, s.field##_bytes, s.field##_pkts, s.field##_pkt_mean)
        PRINT_FLOW("Bhtx_in",  bhtx_in);
        PRINT_FLOW("Bhtx_out", bhtx_out);
        PRINT_FLOW("Bhrx_in",  bhrx_in);
        PRINT_FLOW("Bhrx_out", bhrx_out);
        PRINT_FLOW("Mhtx_in",  mhtx_in);
        PRINT_FLOW("Mhtx_out", mhtx_out);
        PRINT_FLOW("Mhrx_in",  mhrx_in);
        PRINT_FLOW("Mhrx_out", mhrx_out);
        PRINT_FLOW("F1u_rlc",  f1u_rlc);
        PRINT_FLOW("Rlc_f1",   rlc_f1);
        PRINT_FLOW("Rlc_mac",  rlc_mac);
        PRINT_FLOW("Mac_rlc",  mac_rlc);
#undef PRINT_FLOW
        printf("  %-12s  %12lu B  %8u smpl  mean=%8.1f B\n",
               "Mac_dl_bo", s.mac_dl_bo, s.mac_dl_bo_smpl, s.mac_dl_bo_mean);

        printf("\n--- HARQ / CRC ---\n");
        printf("  dl_harq  ack=%-8lu  nack=%-8lu  total=%-8lu  loss=%.3f%%  max_cons=%u\n",
               s.dl_harq_ack, s.dl_harq_nack, s.dl_harq_total,
               s.dl_harq_loss_rate * 100.0, s.dl_harq_max_cons);
        printf("  ul_crc   loss=%-8lu  total=%-8lu  rate=%.3f%%\n",
               s.ul_crc_loss, s.ul_crc_total, s.ul_crc_loss_rate * 100.0);

        printf("\n--- SINR / CSI / BSR aggregates ---\n");
        printf("  Active UEs: %u\n", metrics->active_ue_count);
        printf("  Sched: loop_avg=%.1f us  PRB_util: DL=%.1f%%  UL=%.1f%%\n",
               metrics->sched_loop_avg_us, metrics->dl_prb_utilization_pct, metrics->ul_prb_utilization_pct);
        if (metrics->ue_attach_count > 0 || metrics->ue_detach_count > 0)
            printf("  UE churn: attach=%u  detach=%u\n", metrics->ue_attach_count, metrics->ue_detach_count);
        if (metrics->f1u_dl_loss_rate > 0.001f)
            printf("  F1-U DL loss rate: %.3f%%\n", metrics->f1u_dl_loss_rate * 100.0f);
        printf("  SINR  min=%.2f  max=%.2f  avg=%.2f dB\n",
               s.sinr_min, s.sinr_max, s.sinr_avg);
        printf("  CSI   min=%u    max=%u    avg=%.2f\n",
               s.csi_min, s.csi_max, s.csi_avg);
        printf("  BSR   min=%u  max=%u  avg=%.0f bytes\n",
               s.bsr_min, s.bsr_max, s.bsr_avg);

        printf("\n--- Per-UE MAC aggregates (active UEs: %d) ---\n", s.mac_active_ues);
        printf("  DL TBS (bytes) min=%.0f  max=%.0f  avg=%.0f\n",
               s.mac_dl_tbs_min, s.mac_dl_tbs_max, s.mac_dl_tbs_avg);
        printf("  UL TBS (bytes) min=%.0f  max=%.0f  avg=%.0f\n",
               s.mac_ul_tbs_min, s.mac_ul_tbs_max, s.mac_ul_tbs_avg);
        printf("  DL PRB         min=%.0f  max=%.0f  avg=%.1f\n",
               s.mac_dl_prb_min, s.mac_dl_prb_max, s.mac_dl_prb_avg);
        printf("  PUSCH SNR (dB) min=%.1f  max=%.1f  avg=%.1f\n",
               s.mac_pusch_snr_min, s.mac_pusch_snr_max, s.mac_pusch_snr_avg);
        printf("  DL BLER        min=%.4f  max=%.4f  avg=%.4f\n",
               s.mac_dl_bler_min, s.mac_dl_bler_max, s.mac_dl_bler_avg);

        printf("\n--- Per-UE Phase 1 CSI / beam / UL-health ---\n");
        for (int i = 0; i < MAX_NO_UEs; i++) {
            const aurora_mac_ue_stats_t *u = &metrics->mac_ue_stats[i];
            if (u->rnti == 0) continue;
            printf("  UE rnti=0x%04x\n", u->rnti);
            printf("    CSI: RI=%u PMI(x1=%u,x2=%u) dl_max_mcs=%u\n",
                   u->ri, u->pmi_x1, u->pmi_x2, u->dl_max_mcs);
            printf("    Beam: SSB_RSRP=%d CSI-RS_RSRP=%d SINR_csi=%.1f dB\n",
                   u->ssb_rsrp_best, u->csirs_rsrp_best, (float)u->sinr_csi_x10 / 10.0f);
            if (u->ul_failure || u->pusch_consec_dtx > 3 || u->pucch_consec_dtx > 3) {
                printf("    *** UL Health: ul_failure=%u timer=%u slots, pusch_dtx=%u pucch_dtx=%u ***\n",
                       u->ul_failure, u->ul_failure_timer, u->pusch_consec_dtx, u->pucch_consec_dtx);
            }
            // Phase 2: HARQ round distribution
            int max_rounds = (int)u->dl_num_harq;
            if (max_rounds > 8) max_rounds = 8;
            if (max_rounds > 0) {
                printf("    DL HARQ rounds: ");
                for (int r = 0; r < max_rounds; r++)
                    printf("[%d]=%u ", r, u->dl_harq_rounds[r]);
                printf("  errors=%lu  UL DTX=%u\n", (unsigned long)u->dl_harq_errors, u->ulsch_dtx);
            }
            // Phase 3: starvation events (cumulative — only print when non-zero)
            if (u->dl_starvation_events > 0 || u->ul_starvation_events > 0)
                printf("    Starvation: DL=%u  UL=%u intervals\n",
                       u->dl_starvation_events, u->ul_starvation_events);
        }

        printf("\n--- Per-bearer RLC (active bearers: %d) ---\n", s.rlc_active_bearers);
        printf("  TX PDU bytes  min=%.0f  max=%.0f  avg=%.0f\n",
               s.rlc_txpdu_bytes_min, s.rlc_txpdu_bytes_max, s.rlc_txpdu_bytes_avg);
        // Phase 4: per-RB mode always shown; AM ARQ depth detail only printed for
        // bearers with activity (mode: 0=AM, 1=UM, 2=TM — see aurora.h/aurora_rlc.c).
        for (int i = 0; i < MAX_NO_UEs; i++) {
            for (int rb = 0; rb < MAX_RBS_PER_UE; rb++) {
                const aurora_rlc_radio_bearer_stats_t *rb_s = &metrics->rlc_rb_stats[i][rb];
                if (rb_s->rnti == 0) continue;
                static const char *mode_name[] = {"AM", "UM", "TM"};
                const char *mname = (rb_s->mode < 3) ? mode_name[rb_s->mode] : "?";
                const char *kind = (rb < 4) ? "SRB" : "DRB";
                printf("      %s%u (rb_idx=%d) mode=%s(%u) rxpdu_dd=%lu rxpdu_ow=%lu rxpdu_lost=%lu\n",
                       kind, rb_s->rbid, rb, mname, rb_s->mode,
                       (unsigned long)rb_s->rxpdu_dd_pkts,
                       (unsigned long)rb_s->rxpdu_ow_pkts,
                       (unsigned long)rb_s->rxpdu_lost_pkts);
                if (rb_s->txpdu_retx_pkts > 0 || rb_s->txpdu_dd_pkts > 0 || rb_s->retransmit_list_depth > 0) {
                    printf("      %s%u: retx_pkts=%u retx_bytes=%u dd_pkts=%u dd_bytes=%u "
                           "seg=%u txbuf=%u rxbuf=%u retx_list=%u stalls=%u\n",
                           kind, rb_s->rbid,
                           rb_s->txpdu_retx_pkts, rb_s->txpdu_retx_bytes,
                           rb_s->txpdu_dd_pkts, rb_s->txpdu_dd_bytes,
                           rb_s->txpdu_segmented,
                           rb_s->txbuf_occ_bytes, rb_s->rxbuf_occ_bytes,
                           rb_s->retransmit_list_depth,
                           rb_s->tx_window_stall_events);
                }
            }
        }

        printf("\n--- Per-bearer PDCP (active bearers: %d) ---\n", s.pdcp_active_bearers);
        printf("  TX PDU bytes  min=%.0f  max=%.0f  avg=%.0f\n",
               s.pdcp_txpdu_bytes_min, s.pdcp_txpdu_bytes_max, s.pdcp_txpdu_bytes_avg);
        printf("  TX proc latency (us, not HOL delay -- PDCP has no TX queue)  min=%.1f  max=%.1f  avg=%.1f\n",
               s.pdcp_tx_proc_latency_us_min, s.pdcp_tx_proc_latency_us_max, s.pdcp_tx_proc_latency_us_avg);

        printf("\n--- GTP tunnels (active: %d) ---\n", s.gtp_active_tunnels);
        printf("  TX bytes  min=%.0f  max=%.0f  avg=%.0f\n",
               s.gtp_tx_bytes_min, s.gtp_tx_bytes_max, s.gtp_tx_bytes_avg);
        for (int i = 0; i < MAX_NO_UEs; i++) {
            for (int t = 0; t < MAX_RBS_PER_UE; t++) {
                const aurora_gtp_tunnel_stats_t *tun = &metrics->gtp_tunnel_stats[i][t];
                if (tun->tx_pkts > 0 || tun->rx_pkts > 0) {
                    printf("    GTP tunnel RNTI=%u: tx=%lu rx=%lu tx_loss=%.3f%% rx_loss=%.3f%%\n",
                           tun->rnti, (unsigned long)tun->tx_pkts, (unsigned long)tun->rx_pkts,
                           tun->tx_loss_rate * 100.0f, tun->rx_loss_rate * 100.0f);
                }
            }
        }

        printf("\n--- RC cell 0 ---\n");
        printf("  RSRP=%.2f dBm  SINR=%.2f dB  load=%u%%  intf=%u\n",
               s.rc0_rsrp_avg, s.rc0_sinr_avg, s.rc0_cell_load_pct, s.rc0_interference_lvl);
        printf("  HO attempts=%u  success=%u  rate=%.3f\n",
               s.rc0_ho_attempts, s.rc0_ho_success, s.rc0_ho_success_rate);

        printf("\n--- Process resource usage (delta) ---\n");
        printf("  user_cpu=%lu us  sys_cpu=%lu us  rss=%lu kB\n",
               s.ru_utime_us, s.ru_stime_us, s.ru_maxrss_kb);

        // KPM metrics
        if (metrics->kpm_metric_count > 0) {
            printf("\n  --- KPM Metrics (%u entries) ---\n", metrics->kpm_metric_count);
            for (uint32_t k = 0; k < metrics->kpm_metric_count && k < AURORA_MAX_KPM_METRICS; k++) {
                const aurora_kpm_metric_t *kpm = &metrics->kpm_metrics[k];
                if (kpm->value_type == 0)
                    printf("  [%2u] %-35s = %lu\n", kpm->measurement_id, kpm->name,
                           (unsigned long)kpm->value);
                else
                    printf("  [%2u] %-35s = %.6f\n", kpm->measurement_id, kpm->name,
                           kpm->value_real);
            }
        }

        // ---- RAN Resiliency Summary ----
        printf("\n  === RAN Resiliency Summary ===\n");

        // UL failure detection
        int ues_in_ul_failure = 0;
        for (int i = 0; i < MAX_NO_UEs; i++) {
            if (metrics->mac_ue_stats[i].rnti != 0 && metrics->mac_ue_stats[i].ul_failure)
                ues_in_ul_failure++;
        }
        if (ues_in_ul_failure > 0)
            printf("  [ALARM] %d UE(s) in UL failure state\n", ues_in_ul_failure);
        else
            printf("  [OK]    No UEs in UL failure\n");

        // HARQ health: check if round-3/4 retransmissions are high (>20% of round-0)
        uint32_t total_round0 = 0, total_round3plus = 0;
        for (int i = 0; i < MAX_NO_UEs; i++) {
            if (metrics->mac_ue_stats[i].rnti == 0) continue;
            total_round0    += metrics->mac_ue_stats[i].dl_harq_rounds[0];
            for (int r = 3; r < 8; r++)
                total_round3plus += metrics->mac_ue_stats[i].dl_harq_rounds[r];
        }
        if (total_round0 > 0) {
            float deep_retx_ratio = (float)total_round3plus / (float)total_round0;
            if (deep_retx_ratio > 0.2f)
                printf("  [ALARM] Deep HARQ retx ratio=%.1f%% (round3+/round0)\n",
                       deep_retx_ratio * 100.0f);
            else
                printf("  [OK]    HARQ deep retx ratio=%.1f%%\n", deep_retx_ratio * 100.0f);
        }

        // Scheduler starvation
        int starved_ues = 0;
        for (int i = 0; i < MAX_NO_UEs; i++) {
            if (metrics->mac_ue_stats[i].rnti == 0) continue;
            if (metrics->mac_ue_stats[i].dl_starvation_events > 2 ||
                metrics->mac_ue_stats[i].ul_starvation_events > 2)
                starved_ues++;
        }
        if (starved_ues > 0)
            printf("  [WARN]  %d UE(s) with scheduling starvation (>2 missed intervals)\n", starved_ues);
        else
            printf("  [OK]    No scheduler starvation detected\n");

        // RLC drop rate
        double rlc_drop = 0.0;
        for (uint32_t k = 0; k < metrics->kpm_metric_count; k++) {
            if (metrics->kpm_metrics[k].measurement_id == 13) {
                rlc_drop = metrics->kpm_metrics[k].value_real;
                break;
            }
        }
        if (rlc_drop > 0.01)
            printf("  [ALARM] RLC DL drop rate=%.2f%%\n", rlc_drop * 100.0);
        else
            printf("  [OK]    RLC DL drop rate=%.3f%%\n", rlc_drop * 100.0);

        // SINR
        if (s.sinr_avg < 0.0f)
            printf("  [ALARM] Average SINR=%.1f dB (negative -- severe interference)\n", s.sinr_avg);
        else if (s.sinr_avg < 5.0f)
            printf("  [WARN]  Average SINR=%.1f dB (low)\n", s.sinr_avg);
        else
            printf("  [OK]    Average SINR=%.1f dB\n", s.sinr_avg);

        printf("  ==============================\n");

        /* -------------------------------------------------------
         * Thread runtime metrics
         * ------------------------------------------------------- */
        {
            const aurora_thread_stats_t *ts = &metrics->thread_stats;
            uint32_t nth = (uint32_t)atomic_load_explicit(
                               (const _Atomic uint32_t *)&ts->num_threads,
                               memory_order_relaxed);
            if (nth > 0) {
                printf("\n--- Thread Runtime Metrics (%u monitored threads) ---\n", nth);
                printf("  %-16s  %-6s  %-3s  %-8s  %-10s  %-10s  %-10s"
                       "  %-7s  %-7s  %-8s  %-9s\n",
                       "Thread", "TID", "CPU", "Events", "Max(us)", "Mean(us)",
                       "Std(us)", "Skew", "Kurt", "Outliers", "ctxsw(i)");
                printf("  %-16s  %-6s  %-3s  %-8s  %-10s  %-10s  %-10s"
                       "  %-7s  %-7s  %-8s  %-9s\n",
                       "----------------", "------", "---", "--------",
                       "----------", "----------", "----------",
                       "-------", "-------", "--------", "---------");

                for (uint32_t i = 0; i < nth && i < AURORA_MAX_MONITORED_THREADS; i++) {
                    const aurora_thread_metrics_t *m = &ts->threads[i];
                    if (!m->valid)
                        continue;

                    uint64_t outliers = m->outliers_low + m->outliers_high;
                    printf("  %-16s  %-6d  %-3d  %-8lu  %-10.1f  %-10.1f  %-10.1f"
                           "  %-7.2f  %-7.2f  %-8lu  %-9lu\n",
                           m->thread_name,
                           (int)m->tid,
                           m->cpu_id,
                           (unsigned long)m->total_events,
                           (double)m->max_runtime_ns / 1000.0,
                           m->mean_ns / 1000.0,
                           m->std_deviation_ns / 1000.0,
                           m->skewness,
                           m->kurtosis,
                           (unsigned long)outliers,
                           (unsigned long)m->involuntary_ctxsw);
                }

                /* Summary: flag threads with high involuntary preemptions */
                printf("\n  Real-time health:\n");
                for (uint32_t i = 0; i < nth && i < AURORA_MAX_MONITORED_THREADS; i++) {
                    const aurora_thread_metrics_t *m = &ts->threads[i];
                    if (!m->valid || m->total_events == 0)
                        continue;
                    double outlier_pct = (m->total_events > 0)
                        ? (double)(m->outliers_low + m->outliers_high)
                          * 100.0 / (double)m->total_events
                        : 0.0;
                    if (m->involuntary_ctxsw > 10)
                        printf("  [WARN]  %-16s  preemptions=%lu (involuntary ctxsw)\n",
                               m->thread_name, (unsigned long)m->involuntary_ctxsw);
                    if (outlier_pct > 5.0)
                        printf("  [WARN]  %-16s  runtime_outliers=%.1f%%"
                               " (mean±2σ threshold)\n",
                               m->thread_name, outlier_pct);
                }
            } else {
                printf("\n--- Thread Runtime Metrics: not yet available"
                       " (collection not started or no matching threads)\n");
            }
        }

        return 0;
    }

    /* -----------------------------------------------------------
     * Continuous collection mode
     * ----------------------------------------------------------- */

    /* Build default CSV filename from start timestamp if not provided */
    if (csv_path[0] == '\0') {
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        snprintf(csv_path, sizeof(csv_path),
                 "aurora_metrics_%04d%02d%02d_%02d%02d%02d.csv",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    }

    /* Prefix the CSV basename with the RAN type (e.g. "du_aurora_metrics_...csv")
     * unless the basename already begins with "<rp>_" to avoid duplication. */
    {
        static const char *ran_prefixes[] = {"gnb", "du", "cu", "cu-cp", "cu-up"};
        const char *rp = (aurora_cfg.ran_type <= RAN_CU_UP)
                         ? ran_prefixes[aurora_cfg.ran_type] : "gnb";
        const char *slash = strrchr(csv_path, '/');
        const char *base  = slash ? slash + 1 : csv_path;
        size_t rplen = strlen(rp);
        int already_prefixed = (strncmp(base, rp, rplen) == 0 && base[rplen] == '_');
        if (!already_prefixed) {
            char tmp[sizeof(csv_path)];
            if (slash)
                snprintf(tmp, sizeof(tmp), "%.*s%s_%s",
                         (int)(base - csv_path), csv_path, rp, base);
            else
                snprintf(tmp, sizeof(tmp), "%s_%s", rp, csv_path);
            memcpy(csv_path, tmp, sizeof(csv_path));
        }
    }

    /* Open CSV file — write header immediately */
    FILE *fp = fopen(csv_path, "w");
    if (!fp) {
        perror("aurora_reader: cannot open CSV file");
        return 1;
    }
    write_csv_header(fp, metrics);
    fflush(fp);

    /* Install signal handler for graceful shutdown */
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    /* Batch buffer — allocated once on the heap */
    snapshot_t *batch = calloc((size_t)batch_size, sizeof(snapshot_t));
    if (!batch) {
        fprintf(stderr, "aurora_reader: out of memory for batch buffer\n");
        fclose(fp);
        return 1;
    }

    fprintf(stderr,
            "aurora_reader: writing to '%s'  interval=%u ms  flush every %d samples\n"
            "  Press Ctrl-C to stop.\n",
            csv_path, aurora_cfg.collection_interval_ms, batch_size);

    int      batch_count  = 0;
    uint64_t total_rows   = 0;
    int      flush_count  = 0;
    uint64_t last_ts      = 0;

    unsigned int poll_us = aurora_cfg.collection_interval_ms * 1000u;

    while (g_running) {
        usleep(poll_us);

        /* Skip if shared memory hasn't been updated since last read */
        if (metrics->timestamp_us == last_ts)
            continue;
        last_ts = metrics->timestamp_us;

        fill_snapshot(&batch[batch_count], metrics);
        batch_count++;
        total_rows++;

        if (batch_count >= batch_size) {
            /* Flush batch to CSV */
            for (int i = 0; i < batch_count; i++)
                write_csv_row(fp, &batch[i]);
            fflush(fp);
            flush_count++;
            fprintf(stderr, "  [flush #%d]  %lu rows written  last_ts=%lu\n",
                    flush_count, total_rows, last_ts);
            batch_count = 0;
        }
    }

    /* Flush any remaining partial batch */
    if (batch_count > 0) {
        for (int i = 0; i < batch_count; i++)
            write_csv_row(fp, &batch[i]);
        fflush(fp);
        flush_count++;
        fprintf(stderr, "  [final flush #%d]  %d rows\n", flush_count, batch_count);
    }

    fclose(fp);
    free(batch);

    /* Shut down infrastructure metrics collector if running */
    if (g_infra_cfg.enabled)
        aurora_infra_shutdown();

    fprintf(stderr, "aurora_reader: done.  Total rows=%lu  Flushes=%d  CSV='%s'\n",
            total_rows, flush_count, csv_path);
    return 0;
}
