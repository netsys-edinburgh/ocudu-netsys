/*
 * AURORA shared-memory layout (OCUDU port).
 *
 * Ported from OAI openair2/AURORA/aurora.h. The struct layout of `struct aurora_metrics`
 * is kept byte-identical to the OAI version so that aurora_reader and any other
 * external SysV-SHM consumer can attach to an OCUDU process unchanged.
 *
 * Plain C header: included by the C reader/collectors and by the C++ core (lib/aurora/aurora.cpp).
 *
 * OCUDU UE key note: OAI indexes per-UE arrays by `rnti % MAX_NO_UEs`. OCUDU has no RNTI in
 * RLC/PDCP/F1-U/GTP-U, so the OCUDU producers index per-UE arrays by UE index
 * (du_ue_index on DU/gNB, cu_up_ue_index on CU-UP); the `rnti` field holds the C-RNTI where
 * the producing layer knows it, otherwise (CU-UP) the CU-UP UE index.
 */

#ifndef OCUDU_AURORA_SHM_H
#define OCUDU_AURORA_SHM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/ipc.h>
#include "thread/aurora_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NO_UEs 100
#define MAX_RBS_PER_UE 36  // 4 SRBs (0-3) + 32 DRBs (max per 3GPP TS 38.413)

// Capacity limits for extended metric categories
#define AURORA_MAX_KPM_METRICS  32
#define AURORA_MAX_RC_CELLS      4

// Histogram for packet/sample size distributions.
// Statistical variants (min, max, mean, IQR, variance, std_dev, skewness, kurtosis, outliers)
// are computed by aurora_reader from the bin counts; only raw bins are stored here.
// Bin boundaries (bytes): [0,64) [64,128) [128,256) [256,512) [512,1K) [1K,2K)
//   [2K,4K) [4K,8K) [8K,16K) [16K,32K) [32K,64K) [64K,128K) [128K,256K) [256K,512K) [512K,1M) [1M,∞)
#define AURORA_HIST_BINS 16

typedef struct {
    uint32_t bins[AURORA_HIST_BINS];  // per-bin sample counts
    uint32_t count;                   // total samples recorded this interval
    uint32_t min_val;                 // exact minimum value seen (bytes)
    uint32_t max_val;                 // exact maximum value seen (bytes)
    uint64_t sum;                     // exact sum of values seen (bytes); mean = sum/count.
                                       // The bin-midpoint mean derivable from bins[] alone can
                                       // fall outside [min_val, max_val] when few samples land
                                       // in a wide high-numbered bin -- tracking the exact sum
                                       // is nearly free and avoids reporting an impossible mean.
} aurora_hist_t;


// Shift-type histogram (Procedure-2): bucket = 2^shift; shift=2 for MCS (BUCKET=4), shift=5 for PRB (BUCKET=32)

/* ---------------------------------------------------------------
 * Per-UE FAPI metrics
 * Per-RNTI snapshot of the last scheduled DL/UL PDU parameters
 * (MCS, PRB count, TBS). Indexed by rnti % MAX_NO_UEs.
 * --------------------------------------------------------------- */
typedef struct {
    uint16_t rnti;     // 0 = slot unused
    uint8_t  dl_mcs;
    uint32_t dl_prb;
    uint32_t dl_tbs;
    uint8_t  ul_mcs;
    uint32_t ul_prb;
    uint32_t ul_tbs;
} aurora_fapi_ue_metrics_t;

/* ---------------------------------------------------------------
 * KPM metrics — standard cell-level O-RAN Key Performance Measurements.
 * Automatically recomputed from MAC/PDCP statistics each scheduling round
 * inside aurora_recompute_ue_aggregates(), which also assigns the
 * measurement IDs and names.
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t measurement_id;
    char     name[64];
    uint64_t value;       // integer value (value_type=0)
    double   value_real;  // real value (value_type=1)
    uint8_t  value_type;  // 0=integer, 1=real
} aurora_kpm_metric_t;

/* ---------------------------------------------------------------
 * Radio Control cell metrics.
 * Up to AURORA_MAX_RC_CELLS (4) cells tracked simultaneously.
 * Cell 0 (primary serving cell) is auto-derived from per-UE MAC
 * stats by aurora_recompute_ue_aggregates() each scheduling round.
 * Handover counters are updated via aurora_update_rc_handover().
 * --------------------------------------------------------------- */
typedef struct {
    uint32_t cell_id;
    double   rsrp_avg;             // dBm (averaged over all active UEs)
    double   rsrq_avg;             // dB  (reserved; not tracked in OAI MAC)
    double   sinr_avg;             // dB
    uint32_t handover_attempts;
    uint32_t handover_success;
    double   handover_success_rate;// 0.0–1.0
    uint32_t cell_load_percent;    // total DL PRBs / max PRBs × 100, rounded
    uint32_t interference_level;   // 0=low 1=medium 2=high (derived from SINR), 3=unknown (no active UE)
} aurora_rc_stats_t;

typedef enum {
    RAN_GNB,
    RAN_DU,
    RAN_CU,
    RAN_CU_CP,
    RAN_CU_UP
} ran_type_t;

typedef struct {
    uint32_t collection_interval_ms;
    key_t shm_key;
    ran_type_t ran_type;
    aurora_thread_config_t thread_cfg;  /* Thread metrics collection config */
} aurora_config_t;


typedef struct {
    uint64_t dl_aggr_tbs;
    uint64_t ul_aggr_tbs;
    uint64_t dl_aggr_bytes_sdus;
    uint64_t ul_aggr_bytes_sdus;
    uint64_t dl_curr_tbs;
    uint64_t ul_curr_tbs;
    uint64_t dl_sched_rb;
    uint64_t ul_sched_rb;

    float pusch_snr;
    float pucch_snr;

    float dl_bler;
    float ul_bler;

    uint32_t dl_harq[5];
    uint32_t ul_harq[5];
    uint32_t dl_num_harq;
    uint32_t ul_num_harq;

    uint32_t rnti;
    uint32_t dl_aggr_prb;
    uint32_t ul_aggr_prb;
    uint32_t dl_aggr_sdus;
    uint32_t ul_aggr_sdus;
    uint32_t dl_aggr_retx_prb;
    uint32_t ul_aggr_retx_prb;

    uint32_t bsr;
    uint16_t frame;
    uint16_t slot;

    uint8_t wb_cqi;
    uint8_t dl_mcs1;
    uint8_t ul_mcs1;
    uint8_t dl_mcs2;
    uint8_t ul_mcs2;
    int8_t phr;
    float rsrp_avg;   // Average RSRP (dBm) from CSI-RS/SSB measurements

    /* --- Phase 1: CSI spatial quality (from sched_ctrl->CSI_report) --- */
    uint8_t  ri;                  // Rank Indicator from CRI-RI-LI-PMI-CQI report
    uint8_t  pmi_x1;              // Precoding Matrix Indicator wideband i1
    uint8_t  pmi_x2;              // Precoding Matrix Indicator wideband i2
    uint8_t  dl_max_mcs;          // Scheduler-estimated max DL MCS (from dl_max_mcs)

    /* --- Phase 1: SSB / CSI-RS beam quality --- */
    int16_t  ssb_rsrp_best;       // Best SSB RSRP (raw integer, from CSI report)
    int16_t  csirs_rsrp_best;     // Best CSI-RS RSRP (raw integer, from CSI report)
    int16_t  sinr_csi_x10;        // SINR×10 dB from SSB RSRP report (r[0].SINRx10)

    /* --- Phase 1: UL failure / DTX indicators (RLF precursors) --- */
    uint16_t pusch_consec_dtx;    // Consecutive PUSCH DTX count (sched_ctrl->pusch_consecutive_dtx_cnt)
    uint16_t pucch_consec_dtx;    // Consecutive PUCCH DTX count (sched_ctrl->pucch_consecutive_dtx_cnt)
    uint8_t  ul_failure;          // UL failure flag (sched_ctrl->ul_failure)
    uint32_t ul_failure_timer;    // Slots since UL failure declared (sched_ctrl->ul_failure_timer)

    /* --- Phase 2: Per-interval HARQ round distribution (resiliency indicators) --- */
    uint32_t dl_harq_rounds[8];   // Per-interval DL transmissions per HARQ round (round 0 = initial tx)
    uint32_t ul_harq_rounds[8];   // Per-interval UL transmissions per HARQ round
    uint64_t dl_harq_errors;      // Per-interval DL MAC scheduler errors (stats->dl.errors delta)
    uint64_t ul_harq_errors;      // Per-interval UL MAC scheduler errors (stats->ul.errors delta)
    uint32_t ulsch_dtx;           // Per-interval UL HARQ DTX count (stats->ulsch_DTX delta)

    /* --- Phase 3: Scheduler starvation indicators --- */
    uint32_t dl_starvation_events; // Intervals where this UE received zero DL bytes (cumulative)
    uint32_t ul_starvation_events; // Intervals where this UE received zero UL bytes (cumulative)
} aurora_mac_ue_stats_t;


// RLC Radio Bearer Statistics structure
typedef struct {
    uint32_t rnti;
    uint8_t rbid;
    uint32_t mode;  // 0=AM, 1=UM, 2=TM

    // TX statistics
    uint64_t txpdu_pkts;
    uint64_t txpdu_bytes;
    uint64_t txsdu_pkts;
    uint64_t txsdu_bytes;

    // RX statistics
    uint64_t rxpdu_pkts;
    uint64_t rxpdu_bytes;
    uint64_t rxsdu_pkts;
    uint64_t rxsdu_bytes;

    // Error statistics
    uint64_t rxpdu_lost_pkts;
    uint64_t rxpdu_dd_pkts;
    uint64_t rxpdu_ow_pkts;  // out-of-window RX PDU count (OAI RLC has no distinct out-of-order counter)

    // Additional fields to match E2AP structure
    uint32_t txsdu_wt_us;  // HOL delay in microseconds (TX direction -- oldest item in tx_list/retransmit_list)
    uint32_t rxsdu_wt_us;  // RX-direction counterpart -- reorder/reassembly wait of the oldest pending PDU segment; always 0 for TM (no reordering)

    /* --- Phase 4: RLC AM ARQ depth metrics --- */
    uint32_t txpdu_retx_pkts;        // AM retransmitted PDUs this interval (delta)
    uint32_t txpdu_retx_bytes;       // AM retransmitted bytes this interval (delta)
    uint32_t txpdu_dd_pkts;          // TX SDU discards this interval (timer expiry/window full)
    uint32_t txpdu_dd_bytes;         // TX SDU discard bytes this interval
    uint32_t txpdu_segmented;        // Segmentation events this interval (window pressure)
    uint32_t txbuf_occ_bytes;        // Current TX buffer occupancy (gauge, snapshot)
    uint32_t rxbuf_occ_bytes;        // Current RX buffer occupancy (gauge, snapshot)
    uint16_t retransmit_list_depth;  // AM retransmit list length (snapshot; 0 for UM/TM)
    uint32_t tx_window_stall_events; // Estimated window stall count (retransmit_list non-empty + tx buffer occupancy > 0)
} aurora_rlc_radio_bearer_stats_t;


// PDCP Radio Bearer Statistics structure
typedef struct {
    uint32_t rnti;
    uint8_t rbid;
    uint32_t mode;  // 0=PDCP AM, 1=PDCP UM, 2=PDCP TM

    // TX statistics
    uint64_t txpdu_pkts;
    uint64_t txpdu_bytes;
    uint64_t txsdu_pkts;
    uint64_t txsdu_bytes;

    // RX statistics
    uint64_t rxpdu_pkts;
    uint64_t rxpdu_bytes;
    uint64_t rxsdu_pkts;
    uint64_t rxsdu_bytes;

    // Error statistics
    uint64_t rxpdu_lost_pkts;
    uint64_t rxpdu_dd_pkts;
    uint64_t rxpdu_oo_pkts;

    // Additional fields to match E2AP structure
    uint32_t txsdu_wt_us;  // HOL delay in microseconds -- always 0 for PDCP: no TX queue exists here (see tx_proc_latency_us below for what PDCP actually tracks)

    // PDCP has no TX-side queue (unlike RLC's tx_list/retransmit_list), so there is no
    // head-of-line wait time to report in txsdu_wt_us above. This is a different quantity:
    // the average per-SDU TX processing cost (header build + cipher + integrity) this
    // interval, measured from nr_pdcp_data_req_{srb,drb}() entry to pre-unlock. Not
    // comparable to RLC's txsdu_wt_us -- do not sum or average the two together.
    uint32_t tx_proc_latency_us;  // avg PDCP TX processing latency this interval, microseconds
} aurora_pdcp_radio_bearer_stats_t;

// GTP Tunnel Statistics structure
typedef struct {
    // Basic tunnel identification
    uint32_t rnti;        // user id
    uint32_t teidgnb;     // tunnel id from gnb
    uint8_t qfi;          // QoS flow indicator
    uint32_t teidupf;     // tunnel id from upf

    // Detailed TX statistics
    uint64_t tx_pkts;     // Total transmitted packets
    uint64_t tx_bytes;    // Total transmitted bytes
    uint64_t tx_gpdupkts; // Transmitted G-PDU packets
    uint64_t tx_err_pkts; // Transmission error packets

    // Detailed RX statistics
    uint64_t rx_pkts;     // Total received packets
    uint64_t rx_bytes;    // Total received bytes
    uint64_t rx_gpdupkts; // Received G-PDU packets
    uint64_t rx_err_pkts; // Reception error packets
    uint64_t rx_unkwn_teid_pkts; // Unknown TEID packets

    // QoS metrics
    uint32_t delay_us;    // End-to-end delay in microseconds -- reserved, not tracked (GTP-U bearer state carries no peer-to-peer packet timestamps); NOT the same quantity as tx_proc_latency_us below
    uint32_t jitter_us;   // Jitter in microseconds -- reserved, not tracked

    // GTP-U has no TX queue (direct sendto(), same architecture as PDCP's TX
    // path), so this is processing latency (gtpv1uSendDirect entry to
    // successful sendto() completion this interval, averaged), not a
    // queuing/transit delay. Distinct from the reserved delay_us above.
    uint32_t tx_proc_latency_us;

    // Buffer occupancy
    uint32_t tx_buf_occ;  // TX buffer occupancy (bytes)
    uint32_t rx_buf_occ;  // RX buffer occupancy (bytes)

    /* --- Phase 5: Derived tunnel health --- */
    float    tx_loss_rate;   // Per-interval TX packet loss rate (tx_err_pkts / (tx_pkts + tx_err_pkts))
    float    rx_loss_rate;   // Per-interval RX packet loss rate (rx_err_pkts / (rx_pkts + rx_err_pkts))
} aurora_gtp_tunnel_stats_t;

/* ===================================================================================================================
 * OCUDU extension (appended after the OAI layout, which stays byte-identical so OAI-built readers keep working on the
 * common prefix). Check `ext.version` / `ext.size` before using it.
 * =================================================================================================================== */
#define AURORA_EXT_VERSION         1
#define AURORA_CSI_MAX_RS          4   /* csi_max_nof_reported_rs */
#define AURORA_CSI_MAX_SUBBANDS    18  /* csi_max_nof_subbands */
#define AURORA_CSI_MAX_T2_LAYERS   2   /* max_nof_typeII_layers */
#define AURORA_CSI_MAX_T2_COEFFS   8   /* max_nof_typeII_coefficients */
#define AURORA_MAX_HO_DUS          16
#define AURORA_CSI_NOT_REPORTED    0xFF

/* PMI codebook of the last CSI report (aurora_csi_ue_stats_t.pmi_type). */
#define AURORA_PMI_NONE            0
#define AURORA_PMI_TWO_PORT        1
#define AURORA_PMI_TYPEI_SP        2   /* Type-I single panel */
#define AURORA_PMI_TYPEII          3

/* Full content of the last decoded UE CSI report (OCUDU csi_report_data) plus interval report counters.
 * Indexed like mac_ue_stats (ue_index % MAX_NO_UEs); rnti == 0 means unused. 8-bit fields set to
 * AURORA_CSI_NOT_REPORTED (0xFF) when the quantity was not part of the last report. */
typedef struct {
    uint32_t rnti;
    uint32_t nof_reports;             /* interval: CSI reports received (valid + invalid) */
    uint32_t nof_invalid_reports;     /* interval: reports flagged invalid (not decoded / DTX) */

    /* CRI / L1-RSRP (cri-RSRP or ssb-Index-RSRP report quantity). */
    uint8_t  nof_rs;                  /* number of reported resources (0..4) */
    uint8_t  rsrp_from_ssb;           /* 1: SSB resource indexes (SSBRI), 0: CSI-RS resource indexes (CRI) */
    uint8_t  cri[AURORA_CSI_MAX_RS];  /* CRI / SSBRI of each reported resource */
    int16_t  l1_rsrp_dbm[AURORA_CSI_MAX_RS]; /* L1-RSRP of each reported resource, dBm [-140, -44] */
    int16_t  l1_rsrp_best_dbm;        /* strongest reported L1-RSRP, dBm (0 if none) */

    /* RI / LI / CQI. */
    uint8_t  ri;                      /* rank indicator 1..8 */
    uint8_t  li;                      /* layer indicator 0..7 */
    uint8_t  wb_cqi_tb1;              /* wideband CQI, first TB, 0..15 */
    uint8_t  wb_cqi_tb2;              /* wideband CQI, second TB, 0..15 */
    uint8_t  nof_subbands_tb1;
    uint8_t  nof_subbands_tb2;
    uint8_t  sb_cqi_diff_tb1[AURORA_CSI_MAX_SUBBANDS]; /* subband differential CQI 0..3 */
    uint8_t  sb_cqi_diff_tb2[AURORA_CSI_MAX_SUBBANDS];

    /* PMI. */
    uint8_t  pmi_type;                /* AURORA_PMI_* */
    uint8_t  pmi_two_port;            /* two-port codebook PMI 0..3 */
    uint8_t  pmi_i11;                 /* Type-I SP / Type-II i1,1 */
    uint8_t  pmi_i12;                 /* Type-I SP i1,2 (0xFF if absent) */
    uint8_t  pmi_i13;                 /* Type-I SP i1,3 (0xFF if absent) */
    uint8_t  pmi_i2;                  /* Type-I SP i2 */
    uint16_t t2_i12;                  /* Type-II i1,2 (beam combination) */
    uint8_t  t2_nof_layers;
    uint8_t  t2_i13[AURORA_CSI_MAX_T2_LAYERS];         /* strongest coefficient per layer */
    uint8_t  t2_nof_coeffs[AURORA_CSI_MAX_T2_LAYERS];
    uint8_t  t2_i14[AURORA_CSI_MAX_T2_LAYERS][AURORA_CSI_MAX_T2_COEFFS]; /* wideband amplitudes */
    uint8_t  t2_i21[AURORA_CSI_MAX_T2_LAYERS][AURORA_CSI_MAX_T2_COEFFS]; /* phases */
    uint8_t  t2_i22[AURORA_CSI_MAX_T2_LAYERS][AURORA_CSI_MAX_T2_COEFFS]; /* subband amplitudes */
    uint8_t  _pad[2];
} aurora_csi_ue_stats_t;

/* Handover executions per source DU (CU-CP mobility manager, keyed by gNB-DU ID). */
typedef struct {
    uint64_t gnb_du_id;
    uint32_t valid;
    uint32_t exec_attempts;           /* interval */
    uint32_t exec_success;            /* interval */
    uint32_t exec_attempts_total;     /* cumulative */
    uint32_t exec_success_total;      /* cumulative */
    uint32_t _pad;
    double   exec_success_rate;       /* exec_success_total / exec_attempts_total */
} aurora_du_ho_stats_t;

struct aurora_metrics_ext {
    uint32_t version;                 /* AURORA_EXT_VERSION */
    uint32_t size;                    /* sizeof(struct aurora_metrics_ext) */

    aurora_csi_ue_stats_t csi[MAX_NO_UEs];

    /* Handover. Executions per source DU; preparations (N2 inter-gNB source side) are not per DU. */
    uint32_t             ho_du_count;
    uint32_t             _pad0;
    aurora_du_ho_stats_t ho_du[AURORA_MAX_HO_DUS];
    uint32_t             ho_prep_attempts;        /* interval */
    uint32_t             ho_prep_success;         /* interval */
    uint32_t             ho_prep_attempts_total;  /* cumulative */
    uint32_t             ho_prep_success_total;   /* cumulative */
};

struct aurora_metrics {
    ran_type_t ran_type;
    uint32_t collection_interval_ms;  // Configured collection interval in milliseconds
    
    // Backhaul traffic metrics (L3 PDCP level) - aggregated across all UEs
    uint64_t bhtx_in_size;   // DL traffic from BH at L3 entering the queue (rxsdu_bytes total)
    uint64_t bhtx_out_size;  // DL traffic at L3 leaving the queue (txsdu_bytes total)
    
    // Backhaul RX traffic metrics (L3 PDCP level) - aggregated across all UEs
    uint64_t bhrx_in_size;   // UL traffic from L3 at exit to core queue (queue an outgoing UL packet for the core into the egress queue)
    uint64_t bhrx_out_size;  // UL traffic at L3 leaving the queue (the UL packet is actually transmitted on the network)
    
    // CU-DU interface traffic metrics (F1 interface) - aggregated across all UEs
    uint64_t mhtx_in_size;   // DL traffic from CU to DU entering the queue (PDCP SDU->PDU conversion)
    uint64_t mhtx_out_size;  // DL traffic from CU to DU leaving the queue (F1 transmission)
    uint64_t mhrx_in_size;   // UL traffic from DU to CU entering the queue (PDCP PDU reception from F1)
    uint64_t mhrx_out_size;  // UL traffic from DU to CU leaving the queue (F1 transmission)
    
    // F1-RLC interface traffic metrics - aggregated across all UEs
    uint64_t f1u_rlc_size;   // Downlink traffic from f1 to rlc
    uint64_t rlc_f1_size;    // Uplink traffic from rlc to f1
    
    // RLC-MAC interface traffic metrics - aggregated across all UEs
    uint64_t rlc_mac_size;   // Downlink traffic from rlc to mac
    uint64_t mac_rlc_size;   // Uplink traffic from mac to rlc
    uint64_t mac_dl_bo;      // DL buffer occupancy in MAC layer
    
    // HARQ metrics (DU side) - aggregated across all UEs
    uint64_t dl_harq_ack;        // Total HARQ ACKs
    uint64_t dl_harq_nack;       // Total HARQ NACKs (NACK + DTX)
    uint64_t dl_harq_total;      // Total HARQ reports
    double dl_harq_loss_rate;    // NACK to total ratio
    uint32_t dl_harq_max_cons;   // Maximum consecutive NACKs
    
    // CRC metrics (DU side, UL) - aggregated across all UEs
    uint64_t ul_crc_loss;        // Total CRC losses
    uint64_t ul_crc_total;       // Total CRC reports
    double ul_crc_loss_rate;     // Loss to total ratio
    
    // FAPI metrics - detailed FAPI-level metrics
    uint32_t fapi_dl_total_pdsch_count;
    uint64_t fapi_dl_total_pdsch_size;  // Accumulator for calculating average
    float fapi_dl_avg_pdsch;
    uint32_t fapi_ul_total_pusch_count;
    uint64_t fapi_ul_total_pusch_size;  // Accumulator for calculating average
    float fapi_ul_avg_pusch;
    uint32_t fapi_ue_count;

    // SINR aggregate metrics: minimum, maximum, and mean SINR (dB) across all active UEs.
    // Computed from per-UE mac_ue_stats[].pusch_snr each scheduling round
    float sinr_min;     // MIN SINR (dB) amongst all active UEs
    float sinr_max;     // MAX SINR (dB) amongst all active UEs
    float sinr_avg;     // Average SINR (dB) across all active UEs

    // CSI (CQI) aggregate metrics: minimum, maximum, and mean wideband CQI across all active UEs.
    // Computed from per-UE mac_ue_stats[].wb_cqi each scheduling round
    uint8_t csi_min;    // MIN wideband CQI reported amongst all active UEs
    uint8_t csi_max;    // MAX wideband CQI reported amongst all active UEs
    float   csi_avg;    // Average wideband CQI across all active UEs

    // BSR aggregate metrics: minimum, maximum, and mean Buffer Status Report value across all active UEs.
    // Computed from per-UE mac_ue_stats[].bsr each scheduling round
    uint32_t bsr_min;   // Min Buffer Status Report value amongst all active UEs
    uint32_t bsr_max;   // Max Buffer Status Report value amongst all active UEs
    float    bsr_avg;   // Average BSR value across all active UEs

    // FAPI DL per-PDU aggregate metrics: min/max/avg of MCS, PRB allocation, and TBS for all DL scheduled PDSCHs.
    // Updated per PDSCH PDU in gNB_scheduler_dlsch.c
    uint8_t  dl_fapi_mcs_max;    // Max MCS amongst all DL scheduled PDSCHs
    uint8_t  dl_fapi_mcs_min;    // Min MCS amongst all DL scheduled PDSCHs
    float    dl_fapi_mcs_avg;    // Average MCS across all DL scheduled PDSCHs
    uint32_t dl_fapi_prb_max;    // Max PRB allocation amongst all DL scheduled PDSCHs
    uint32_t dl_fapi_prb_min;    // Min PRB allocation amongst all DL scheduled PDSCHs
    float    dl_fapi_prb_avg;    // Average PRB allocation for DL
    uint32_t dl_fapi_tbs_max;    // Max TBS amongst all DL scheduled PDSCHs
    uint32_t dl_fapi_tbs_min;    // Min TBS amongst all DL scheduled PDSCHs
    float    dl_fapi_tbs_avg;    // Average TBS for DL (per PDSCH)

    // FAPI UL per-PDU aggregate metrics: min/max/avg of MCS, PRB allocation, and TBS for all UL scheduled PUSCHs.
    // Updated per PUSCH PDU in gNB_scheduler_ulsch.c
    uint8_t  ul_fapi_mcs_max;    // Max MCS amongst all UL scheduled PUSCHs
    uint8_t  ul_fapi_mcs_min;    // Min MCS amongst all UL scheduled PUSCHs
    float    ul_fapi_mcs_avg;    // Average MCS across all UL scheduled PUSCHs
    uint32_t ul_fapi_prb_max;    // Max PRB allocation amongst all UL scheduled PUSCHs
    uint32_t ul_fapi_prb_min;    // Min PRB allocation amongst all UL scheduled PUSCHs
    float    ul_fapi_prb_avg;    // Average PRB allocation for UL
    uint32_t ul_fapi_tbs_max;    // Max TBS amongst all UL scheduled PUSCHs
    uint32_t ul_fapi_tbs_min;    // Min TBS amongst all UL scheduled PUSCHs
    float    ul_fapi_tbs_avg;    // Average TBS for UL (per PUSCH)


    // Packet-size histograms for traffic flow statistical analysis.
    // Each histogram records per-packet (or per-sample) sizes within the collection interval.
    // aurora_reader derives: min, max, mean, range, IQR, variance, std_dev, skewness, kurtosis, outliers.
    aurora_hist_t hist_bhtx_in;    // Bhtx_in  — BH DL ingress packet sizes
    aurora_hist_t hist_bhtx_out;   // Bhtx_out — BH DL egress packet sizes
    aurora_hist_t hist_bhrx_in;    // Bhrx_in  — BH UL ingress packet sizes
    aurora_hist_t hist_bhrx_out;   // Bhrx_out — BH UL egress packet sizes
    aurora_hist_t hist_mhtx_in;    // Mhtx_in  — MH DL (CU→DU) ingress PDU sizes
    aurora_hist_t hist_mhtx_out;   // Mhtx_out — MH DL (CU→DU) egress PDU sizes
    aurora_hist_t hist_mhrx_in;    // Mhrx_in  — MH UL (DU→CU) ingress PDU sizes
    aurora_hist_t hist_mhrx_out;   // Mhrx_out — MH UL (DU→CU) egress PDU sizes
    aurora_hist_t hist_f1u_rlc;    // F1u_rlc  — F1-U→RLC DL PDU sizes
    aurora_hist_t hist_rlc_f1;     // Rlc_f1   — RLC→F1-U UL PDU sizes
    aurora_hist_t hist_rlc_mac;    // Rlc_mac  — RLC→MAC DL PDU sizes
    aurora_hist_t hist_mac_rlc;    // Mac_rlc  — MAC→RLC UL PDU sizes
    aurora_hist_t hist_mac_dl_bo;  // Mac_dl_bo — MAC DL buffer occupancy samples

    // Shift-type histograms for FAPI MCS/PRB (used by Procedure-2 in aurora_reader)
    // DL MCS: shift=2 (BUCKET=4), bin i covers [4*i, 4*(i+1)-1]; 16 bins → [0,3]..[60,63]
    aurora_hist_t hist_dl_fapi_mcs;
    // DL PRB: shift=5 (BUCKET=32), bin i covers [32*i, 32*(i+1)-1]; 16 bins → [0,31]..[480,511]
    aurora_hist_t hist_dl_fapi_prb;
    // UL MCS: shift=2 (BUCKET=4)
    aurora_hist_t hist_ul_fapi_mcs;
    // UL PRB: shift=5 (BUCKET=32)
    aurora_hist_t hist_ul_fapi_prb;

    /* -------------------------------------------------------
     * Per-UE FAPI metrics
     * Per-RNTI snapshot of the last scheduled DL/UL PDU parameters
     * (MCS, PRB count, TBS). Indexed by rnti % MAX_NO_UEs; rnti=0 means slot unused.
     * ------------------------------------------------------- */
    aurora_fapi_ue_metrics_t fapi_ue_metrics[MAX_NO_UEs];

    /* -------------------------------------------------------
     * KPM cell-level metrics (O-RAN WG3 Key Performance Measurements)
     * 12 standard O-RAN measurements automatically recomputed in
     * aurora_recompute_ue_aggregates() from MAC/PDCP aggregates.
     * ------------------------------------------------------- */
    uint32_t kpm_metric_count;
    aurora_kpm_metric_t kpm_metrics[AURORA_MAX_KPM_METRICS];

    /* -------------------------------------------------------
     * Radio Control cell metrics
     * Per-cell RSRP, SINR, load percentage, and interference level derived
     * from UE aggregates. Handover attempt/success counters updated via
     * aurora_update_rc_handover().
     * ------------------------------------------------------- */
    uint32_t rc_cell_count;
    aurora_rc_stats_t rc_metrics[AURORA_MAX_RC_CELLS];

    /* --- Phase 1: Active UE count (recomputed each interval) --- */
    uint32_t active_ue_count;     // Number of UEs with non-zero RNTI this interval

    /* --- Phase 3: Cell-level scheduler health --- */
    float    sched_loop_avg_us;      // Average scheduler loop execution time (microseconds)
    uint64_t sched_loop_max_us;      // Worst-case scheduler loop execution time (microseconds)
    float    dl_prb_utilization_pct; // DL PRB utilization % this interval (total_dl_prbs / max_prbs)
    float    ul_prb_utilization_pct; // UL PRB utilization % this interval (total_ul_prbs / max_prbs)

    /* --- Phase 5: Interface health and UE churn --- */
    float    f1u_dl_loss_rate;  // DL F1-U loss rate estimate (f1u_rlc_size vs rlc_mac_size delta ratio)
    uint32_t ue_attach_count;   // UE attach/context-setup events this interval
    uint32_t ue_detach_count;   // UE detach/release events this interval
    uint32_t du_count;          // CU-side: number of DUs currently connected (RRC rrc->num_dus
                                 // gauge, mirrored here via aurora_update_du_count() -- see
                                 // rrc_cell_management.c's rrc_add_du()/rrc_rm_du())

    aurora_mac_ue_stats_t mac_ue_stats[MAX_NO_UEs]; // Store stats for up to 'MAX_NO_UEs' UEs (indexed by RNTI)
    aurora_rlc_radio_bearer_stats_t rlc_rb_stats[MAX_NO_UEs][MAX_RBS_PER_UE]; // Store stats for up to 'MAX_NO_UEs' UEs and MAX_RBS_PER_UE RBs per UE
    aurora_pdcp_radio_bearer_stats_t pdcp_rb_stats[MAX_NO_UEs][MAX_RBS_PER_UE]; // Store stats for up to 'MAX_NO_UEs' UEs and MAX_RBS_PER_UE RBs per UE
    aurora_gtp_tunnel_stats_t gtp_tunnel_stats[MAX_NO_UEs][MAX_RBS_PER_UE]; // Store GTP tunnel stats for up to 'MAX_NO_UEs' UEs and MAX_RBS_PER_UE RBs per UE
    
    uint64_t timestamp_us;  // Timestamp in microseconds since epoch when metrics were last updated

    /* Process resource usage (as reported by getrusage()) */
    uint64_t ru_utime_us;    // User CPU time in microseconds
    uint64_t ru_stime_us;    // System CPU time in microseconds
    uint64_t ru_maxrss_kb;   // Maximum resident set size (kB)
    uint64_t ru_minflt;      // Page reclaims (minor faults)
    uint64_t ru_majflt;      // Page faults (major)
    uint64_t ru_inblock;     // Block input operations
    uint64_t ru_oublock;     // Block output operations
    uint64_t ru_nvcsw;       // Voluntary context switches
    uint64_t ru_nivcsw;      // Involuntary context switches

    /* -------------------------------------------------------
     * Per-thread runtime metrics (Phase 2 extension)
     * Scheduling latency histograms and derived statistics for
     * all named worker threads (up to AURORA_MAX_MONITORED_THREADS).
     * Populated by the aurora_thread procfs/eBPF collector.
     * ------------------------------------------------------- */
    aurora_thread_stats_t thread_stats;

    /* OCUDU extension, see struct aurora_metrics_ext. Must stay the last member. */
    struct aurora_metrics_ext ext;
};


void aurora_hist_record(aurora_hist_t *hist, uint32_t val);
void aurora_hist_record_shift(aurora_hist_t *hist, uint32_t val, int shift);

/* Attach read-only (SHM_RDONLY, consumer side) to an existing segment. Returns NULL on failure. */
struct aurora_metrics *aurora_get_metrics_by_key(key_t shm_key);

/* Wall-clock timestamp in microseconds since epoch. */
uint64_t aurora_get_current_timestamp_us(void);

#ifdef __cplusplus
}
#endif

#endif /* OCUDU_AURORA_SHM_H */
