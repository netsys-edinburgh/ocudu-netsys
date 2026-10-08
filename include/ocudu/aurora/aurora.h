// AURORA RAN metrics framework (port of OAI openair2/AURORA).
//
// Producers anywhere in the stack (MAC/scheduler, RLC, PDCP, F1-U, GTP-U, CU-CP) call the hooks
// below. Every hook is a cheap no-op until the application calls aurora::start(). Hooks only touch
// process-private relaxed atomics; a background publisher thread ("aurora_reset") folds them every
// collection interval into the SysV shared-memory `struct aurora_metrics` (lib/aurora/aurora_shm.h),
// whose layout is identical to the OAI one so the same aurora_reader works unchanged.
//
// UE key: OAI uses `rnti % 100`. OCUDU RLC/PDCP/F1-U/GTP-U have no RNTI, so per-UE slots are indexed by
// UE index (DU: du_ue_index, CU-UP: cu_up_ue_index) modulo max_nof_ues.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace ocudu {
namespace aurora {

/// Same numeric values as OAI `ran_type_t` (shared memory field).
enum class ran_type : int { gnb = 0, du = 1, cu = 2, cu_cp = 3, cu_up = 4 };

/// Same numeric values as `aurora_thread_backend_t`.
enum class thread_backend : int { automatic = 0, ebpf = 1, procfs = 2 };

/// Capacity limits, identical to the OAI shared-memory layout.
constexpr unsigned max_nof_ues        = 100;
constexpr unsigned max_rbs_per_ue     = 36; ///< 4 SRBs (0-3) + 32 DRBs (4-35).
constexpr unsigned max_harq_rounds    = 8;
constexpr unsigned first_drb_rb_index = 4;

struct config {
  bool                      enabled = false;
  ran_type                  type    = ran_type::gnb;
  std::chrono::milliseconds collection_interval{100};
  uint32_t                  shm_key = 0xdeadbeef;
  bool                      thread_metrics_enabled = true;
  thread_backend            thread_metrics_backend = thread_backend::automatic;
  /// Comma-separated thread names; trailing '*' is a prefix wildcard.
  std::string thread_metrics_whitelist;
  /// Anomaly-injection TCP control server (port defaults per role: 47010 + ran_type).
  bool anomaly_enabled = true;
};

/// Default OCUDU thread whitelist (OS thread names created by worker_manager / io_broker / remote server).
const char* default_thread_whitelist();

/// Creates the shared memory segment and starts the publisher, thread collector and anomaly server.
/// Returns 0 on success. Fails if another live process is attached to a segment with the same key.
int start(const config& cfg);
/// Stops all AURORA threads, detaches and removes the shared memory segment (as OAI aurora_cleanup()).
void stop();

namespace detail {
extern std::atomic<bool> active;
} // namespace detail

/// True once start() succeeded. Hooks may use it to skip computing expensive arguments.
inline bool is_active() noexcept
{
  return detail::active.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------------------------------------------------
// Group 1 - traffic flows (bytes + per-packet size histogram).
// ---------------------------------------------------------------------------------------------------------------------

enum class flow : unsigned {
  bhtx_in = 0, ///< DL: N3 GTP-U PDU received from core, queued.
  bhtx_out,    ///< DL: N3 PDU dequeued towards SDAP/PDCP.
  bhrx_in,     ///< UL: GTP-U PDU queued for transmission to core.
  bhrx_out,    ///< UL: GTP-U PDU transmitted to core.
  mhtx_in,     ///< DL: PDCP PDU queued for F1-U transmission.
  mhtx_out,    ///< DL: F1-U PDU transmitted to DU.
  mhrx_in,     ///< UL: F1-U PDU received from DU, queued.
  mhrx_out,    ///< UL: F1-U PDU dequeued towards PDCP.
  f1u_rlc,     ///< DL: F1-U PDU delivered to RLC (DU).
  rlc_f1,      ///< UL: RLC SDU submitted to F1-U (DU).
  rlc_mac,     ///< DL: RLC PDU pulled by MAC.
  mac_rlc,     ///< UL: MAC SDU delivered to RLC.
  mac_dl_bo,   ///< DL buffer occupancy samples reported to MAC.
  nof_flows
};

void record_flow(flow f, uint64_t nof_bytes) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// MAC / scheduler hooks (DU, gNB). Groups 2-8, 11, 12.
// ---------------------------------------------------------------------------------------------------------------------

enum class harq_ack_status { ack, nack, dtx };

struct grant_info {
  unsigned ue_index   = 0;
  uint16_t rnti       = 0;
  unsigned mcs        = 0;
  unsigned mcs_table  = 0; ///< 0 = qam64, 1 = qam256, 2 = qam64LowSe ... (OAI mcsTableIdx semantics).
  unsigned nof_prbs   = 0;
  unsigned tbs_bytes  = 0;
  unsigned nof_retxs  = 0; ///< 0 = new transmission.
  unsigned sfn        = 0;
  unsigned slot_index = 0;
};

void on_ue_created(unsigned ue_index, uint16_t rnti) noexcept;
void on_ue_removed(unsigned ue_index) noexcept;
void on_pdsch_grant(const grant_info& grant) noexcept;
void on_pusch_grant(const grant_info& grant) noexcept;
void on_dl_harq_feedback(unsigned ue_index, uint16_t rnti, unsigned harq_id, harq_ack_status status) noexcept;
/// PUSCH CRC indication. `sinr_db` is the PUSCH SINR estimate, when available.
void on_ul_crc(unsigned ue_index, uint16_t rnti, bool crc_ok, std::optional<float> sinr_db) noexcept;
/// PUSCH expected but not detected (UL HARQ DTX).
void on_ul_dtx(unsigned ue_index) noexcept;
/// HARQ process discarded after reaching max retransmissions without ACK.
void on_harq_max_retx_failure(unsigned ue_index, bool is_dl) noexcept;
void on_pucch_snr(unsigned ue_index, float snr_db) noexcept;

/// Full content of one decoded UE CSI report (OCUDU csi_report_data: CRI/RI/LI/PMI/CQI and L1-RSRP quantities).
struct csi_info {
  static constexpr unsigned max_rs        = 4;  ///< csi_max_nof_reported_rs.
  static constexpr unsigned max_subbands  = 18; ///< csi_max_nof_subbands.
  static constexpr unsigned max_t2_layers = 2;  ///< max_nof_typeII_layers.
  static constexpr unsigned max_t2_coeffs = 8;  ///< max_nof_typeII_coefficients.

  enum class pmi_kind : uint8_t { none = 0, two_port = 1, typeI_single_panel = 2, typeII = 3 };

  /// False if the report could not be decoded (counted, content ignored).
  bool valid = true;

  // CRI / SSBRI and L1-RSRP of each reported resource.
  unsigned                      nof_rs = 0;
  std::array<uint8_t, max_rs>   cri{};
  std::array<int16_t, max_rs>   l1_rsrp_dbm{};
  /// True if the reported resources are SSBs (ssb-Index-RSRP), false for CSI-RS (cri-RSRP).
  bool rsrp_from_ssb = false;

  std::optional<unsigned> ri;
  std::optional<unsigned> li;
  std::optional<unsigned> wb_cqi;     ///< First TB.
  std::optional<unsigned> wb_cqi_tb2; ///< Second TB.
  unsigned                               nof_subbands_tb1 = 0;
  unsigned                               nof_subbands_tb2 = 0;
  std::array<uint8_t, max_subbands>      sb_cqi_diff_tb1{};
  std::array<uint8_t, max_subbands>      sb_cqi_diff_tb2{};

  pmi_kind                pmi = pmi_kind::none;
  unsigned                pmi_two_port = 0;
  std::optional<unsigned> pmi_i1;  ///< i1,1 (Type-I SP / Type-II).
  std::optional<unsigned> pmi_i12; ///< Type-I SP i1,2.
  std::optional<unsigned> pmi_i13; ///< Type-I SP i1,3.
  std::optional<unsigned> pmi_i2;  ///< Type-I SP i2 (two-port PMI for the two-port codebook).
  unsigned                t2_i12        = 0;
  unsigned                t2_nof_layers = 0;
  std::array<uint8_t, max_t2_layers>                             t2_i13{};
  std::array<uint8_t, max_t2_layers>                             t2_nof_coeffs{};
  std::array<std::array<uint8_t, max_t2_coeffs>, max_t2_layers> t2_i14{};
  std::array<std::array<uint8_t, max_t2_coeffs>, max_t2_layers> t2_i21{};
  std::array<std::array<uint8_t, max_t2_coeffs>, max_t2_layers> t2_i22{};

  /// Strongest reported L1-RSRP, if any.
  std::optional<float> l1_rsrp_best_dbm() const
  {
    if (nof_rs == 0) {
      return std::nullopt;
    }
    int16_t best = l1_rsrp_dbm[0];
    for (unsigned i = 1; i < nof_rs && i < max_rs; ++i) {
      best = std::max(best, l1_rsrp_dbm[i]);
    }
    return static_cast<float>(best);
  }
};
void on_csi_report(unsigned ue_index, const csi_info& csi) noexcept;
void on_bsr(unsigned ue_index, uint32_t nof_bytes) noexcept;
void on_phr(unsigned ue_index, int ph_db) noexcept;
/// RLC DL buffer occupancy update of one logical channel. AURORA samples the per-UE total (sum over LCIDs), as OAI
/// update_dlsch_buffer() does.
void on_dl_buffer_state(unsigned ue_index, unsigned lcid, uint32_t nof_bytes) noexcept;
/// MAC SDU (RLC PDU) multiplexed into a DL MAC PDU.
void on_mac_dl_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept;
/// MAC SDU demultiplexed from a UL MAC PDU and delivered to RLC.
void on_mac_ul_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept;
/// Max DL MCS the scheduler may assign to this UE (link adaptation ceiling).
void on_dl_max_mcs(unsigned ue_index, unsigned mcs) noexcept;
/// Consecutive PUSCH/PUCCH KO (DTX/CRC-fail) counters as tracked by the MAC RLF detector.
void on_consecutive_ul_ko(unsigned ue_index, unsigned pusch_kos, unsigned pucch_kos) noexcept;
/// Radio link failure declared (true) or cleared on UE recovery/removal (false).
void on_ul_failure(unsigned ue_index, bool declared) noexcept;
/// One scheduler slot decision for a cell.
void on_scheduler_slot(unsigned pci,
                       std::chrono::microseconds decision_latency,
                       unsigned                  nof_cell_prbs,
                       bool                      is_dl_slot,
                       bool                      is_ul_slot) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// CU-CP hooks. Group 11.
// ---------------------------------------------------------------------------------------------------------------------

/// Handover execution requested / successful, keyed by source gNB-DU ID (per-DU table in the OCUDU extension; the
/// totals also fill RC cell 0 handover counters).
void on_handover_attempt(uint64_t source_gnb_du_id) noexcept;
void on_handover_success(uint64_t source_gnb_du_id) noexcept;
/// N2 (inter-gNB) handover preparation requested / successful on the source side. Not attributable to a DU.
void on_handover_preparation_attempt() noexcept;
void on_handover_preparation_success() noexcept;
void on_du_count(unsigned nof_dus) noexcept;
/// L3 RSRQ (dB) from a UE measurement report of the serving cell.
void on_serving_cell_rsrq(float rsrq_db) noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// RLC bearer hooks (DU, gNB). Group 13.
// ---------------------------------------------------------------------------------------------------------------------

enum class rlc_mode : uint32_t { am = 0, um = 1, tm = 2 }; ///< OAI numeric encoding.

/// Bearer slot index in [0, max_rbs_per_ue): SRB n -> n, DRB n -> 3 + n.
constexpr unsigned srb_rb_index(unsigned srb_id)
{
  return srb_id;
}
constexpr unsigned drb_rb_index(unsigned drb_id)
{
  return first_drb_rb_index - 1 + drb_id;
}

enum class rlc_counter : unsigned {
  txpdu_pkts = 0,
  txpdu_bytes,
  txsdu_pkts,
  txsdu_bytes,
  rxpdu_pkts,
  rxpdu_bytes,
  rxsdu_pkts,
  rxsdu_bytes,
  rxpdu_lost_pkts,
  rxpdu_dd_pkts,
  rxpdu_ow_pkts,
  txpdu_retx_pkts,
  txpdu_retx_bytes,
  txpdu_dd_pkts,
  txpdu_dd_bytes,
  txpdu_segmented,
  tx_window_stall_events,
  nof_counters
};
enum class rlc_gauge : unsigned {
  txsdu_wt_us = 0,
  rxsdu_wt_us,
  txbuf_occ_bytes,
  rxbuf_occ_bytes,
  retransmit_list_depth,
  nof_gauges
};

void rlc_bearer_created(unsigned ue_index, unsigned rb_index, rlc_mode mode) noexcept;
void rlc_add(unsigned ue_index, unsigned rb_index, rlc_counter c, uint64_t value = 1) noexcept;
void rlc_set(unsigned ue_index, unsigned rb_index, rlc_gauge g, uint32_t value) noexcept;
/// ANOMALY injection point on the RLC data path (QUEUE_TUNING jitter).
void rlc_hook_anomaly() noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// PDCP bearer hooks (gNB, CU, CU-UP, CU-CP). Group 14.
// ---------------------------------------------------------------------------------------------------------------------

enum class pdcp_counter : unsigned {
  txpdu_pkts = 0,
  txpdu_bytes,
  txsdu_pkts,
  txsdu_bytes,
  rxpdu_pkts,
  rxpdu_bytes,
  rxsdu_pkts,
  rxsdu_bytes,
  rxpdu_lost_pkts,
  rxpdu_dd_pkts,
  rxpdu_oo_pkts,
  nof_counters
};

void pdcp_bearer_created(unsigned ue_index, unsigned rb_index, rlc_mode mode) noexcept;
void pdcp_bearer_removed(unsigned ue_index, unsigned rb_index) noexcept;
void pdcp_add(unsigned ue_index, unsigned rb_index, pdcp_counter c, uint64_t value = 1) noexcept;
/// TX processing cost of one PDU (header + ciphering + integrity), OAI tx_proc_latency_us.
void pdcp_tx_proc_latency(unsigned ue_index, unsigned rb_index, uint64_t proc_latency_ns) noexcept;
/// TX sojourn of one SDU: arrival at PDCP until the PDU is handed to the lower layers. AURORA reports
/// txsdu_wt_us = avg sojourn - avg processing cost (i.e. queueing in the crypto / UE executors).
void pdcp_tx_sojourn(unsigned ue_index, unsigned rb_index, uint64_t sojourn_ns) noexcept;
/// ANOMALY injection point on the PDCP data path (PDCP_CONTENTION delay, QUEUE_TUNING jitter).
void pdcp_hook_anomaly() noexcept;

// ---------------------------------------------------------------------------------------------------------------------
// GTP-U N3 tunnel hooks (gNB, CU, CU-UP). Group 15. Each tunnel occupies one of the UE's max_rbs_per_ue slots.
// ---------------------------------------------------------------------------------------------------------------------

enum class gtp_counter : unsigned {
  tx_pkts = 0,
  tx_bytes,
  tx_err_pkts,
  rx_pkts,
  rx_bytes,
  rx_err_pkts,
  rx_unkwn_teid_pkts,
  nof_counters
};

/// Allocates a free tunnel slot of the UE. Returns the slot index, or -1 if AURORA is inactive or the UE has no free
/// slot left.
int gtp_tunnel_created(unsigned ue_index, uint32_t local_teid, uint32_t peer_teid) noexcept;
void gtp_tunnel_removed(unsigned ue_index, unsigned tunnel_index) noexcept;
void gtp_set_qfi(unsigned ue_index, unsigned tunnel_index, uint8_t qfi) noexcept;
void gtp_add(unsigned ue_index, unsigned tunnel_index, gtp_counter c, uint64_t value = 1) noexcept;
/// Packet with unknown TEID at the N3 demux (not attributable to a UE).
void gtp_unknown_teid() noexcept;
void gtp_tx_latency(unsigned ue_index, unsigned tunnel_index, uint64_t proc_latency_ns) noexcept;

} // namespace aurora
} // namespace ocudu
