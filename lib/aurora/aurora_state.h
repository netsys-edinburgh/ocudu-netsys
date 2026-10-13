// AURORA process-private accumulators.
//
// Hooks (aurora_hooks.cpp) write these with relaxed atomics from any thread. The publisher thread
// (aurora_publish.cpp) drains them once per collection interval into the shared-memory `aurora_metrics`.

#pragma once

#include "aurora_shm.h"
#include "ocudu/aurora/aurora.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace ocudu {
namespace aurora {
namespace detail {

constexpr unsigned max_harq_ids     = 16;
constexpr unsigned max_rc_cells     = AURORA_MAX_RC_CELLS;
constexpr unsigned nof_hist_bins    = AURORA_HIST_BINS;
constexpr unsigned max_gtp_tunnels  = max_rbs_per_ue;
constexpr uint32_t u32_max          = std::numeric_limits<uint32_t>::max();
constexpr unsigned dl_harq_round_nb = 5; ///< Size of the OAI dl_harq[]/ul_harq[] arrays.
constexpr unsigned max_lcids        = 64;

inline unsigned ue_slot(unsigned ue_index)
{
  return ue_index % max_nof_ues;
}

template <typename T>
inline void atomic_max(std::atomic<T>& a, T v)
{
  T cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

template <typename T>
inline void atomic_min(std::atomic<T>& a, T v)
{
  T cur = a.load(std::memory_order_relaxed);
  while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
  }
}

inline void add(std::atomic<uint64_t>& a, uint64_t v)
{
  a.fetch_add(v, std::memory_order_relaxed);
}
inline void add(std::atomic<uint32_t>& a, uint32_t v)
{
  a.fetch_add(v, std::memory_order_relaxed);
}

template <typename T>
inline void set(std::atomic<T>& a, T v)
{
  a.store(v, std::memory_order_relaxed);
}

template <typename T>
inline T get(const std::atomic<T>& a)
{
  return a.load(std::memory_order_relaxed);
}

template <typename T>
inline T take(std::atomic<T>& a, T reset_value = T{})
{
  return a.exchange(reset_value, std::memory_order_relaxed);
}

/// Interval histogram, same binning as aurora_hist_record()/aurora_hist_record_shift().
struct hist_acc {
  std::array<std::atomic<uint32_t>, nof_hist_bins> bins{};
  std::atomic<uint32_t>                             count{0};
  std::atomic<uint32_t>                             min_val{u32_max};
  std::atomic<uint32_t>                             max_val{0};
  std::atomic<uint64_t>                             sum{0};

  /// shift < 0 selects the exponential byte-size binning, otherwise bucket = 2^shift.
  void record(uint32_t val, int shift = -1);
  /// Moves the interval content into `out` and resets the accumulator.
  void drain(aurora_hist_t& out);
};

/// Running min/max/sum/count of an interval quantity.
struct stat_acc {
  std::atomic<uint32_t> count{0};
  std::atomic<uint64_t> sum{0};
  std::atomic<uint32_t> min_val{u32_max};
  std::atomic<uint32_t> max_val{0};

  void record(uint32_t v)
  {
    add(count, 1U);
    add(sum, uint64_t{v});
    atomic_min(min_val, v);
    atomic_max(max_val, v);
  }
};

/// One F1AP elementary procedure (request / response).
struct f1c_proc_acc {
  std::atomic<uint32_t> attempts{0}, success{0}, failure{0};
  stat_acc              latency_us;
};

/// F1AP request waiting for its outcome, for latency and timeout accounting.
struct f1c_pending_request {
  bool     valid    = false;
  unsigned proc     = 0;
  uint64_t key      = 0;
  uint64_t start_ns = 0;
};

/// F1-C (F1AP) interface accumulators. Index 0 = DL (CU-CP -> DU), 1 = UL.
struct f1c_acc {
  std::array<std::atomic<uint64_t>, 2> msgs{}, bytes{}, rrc_msgs{}, rrc_bytes{};
  std::array<std::atomic<uint32_t>, 2> bytes_max{};
  std::atomic<uint32_t>                decode_errors{0}, init_ul_rrc{0}, paging{0}, ue_ctx_release_req{0}, resets{0},
      error_indications{0}, timeouts{0};
  std::atomic<uint32_t> links_up{0};
  /// Request/response procedures: F1 Setup, UE Context Setup, Modification and Release.
  std::array<f1c_proc_acc, 4> procs;

  static constexpr unsigned                   max_pending = 128;
  std::mutex                                  pending_mutex; ///< F1-C is low rate; hooks run on control executors.
  std::array<f1c_pending_request, max_pending> pending;
};

/// F1-U (NR-U) interface accumulators, indexed by f1u_event.
struct f1u_acc {
  std::array<std::atomic<uint64_t>, static_cast<unsigned>(f1u_event::nof_events)> events{};
};

struct flow_acc {
  std::atomic<uint64_t> bytes{0};
  hist_acc              hist;
};

struct mac_ue_acc {
  std::atomic<bool>     active{false};
  std::atomic<uint32_t> owner{u32_max}; ///< UE index owning the slot (slots are shared modulo max_nof_ues).
  std::atomic<uint16_t> rnti{0};

  // Interval counters.
  std::atomic<uint64_t>                              dl_tbs{0}, ul_tbs{0};
  std::atomic<uint64_t>                              dl_sdu_bytes{0}, ul_sdu_bytes{0};
  std::atomic<uint64_t>                              dl_prbs{0}, ul_prbs{0};
  std::atomic<uint32_t>                              dl_sdus{0}, ul_sdus{0};
  std::atomic<uint32_t>                              dl_retx_prbs{0}, ul_retx_prbs{0};
  std::array<std::atomic<uint32_t>, max_harq_rounds> dl_rounds{}, ul_rounds{};
  std::atomic<uint32_t>                              dl_harq_errors{0}, ul_harq_errors{0};
  std::atomic<uint32_t>                              ulsch_dtx{0};
  std::atomic<uint32_t>                              dl_ok{0}, dl_nok{0}, ul_ok{0}, ul_nok{0};
  std::atomic<bool>                                  dl_granted{false}, ul_granted{false};

  // Last-value gauges.
  std::atomic<float>    pusch_snr{0}, pucch_snr{0}, rsrp_dbm{0};
  std::atomic<uint32_t> wb_cqi{0}, ri{0}, pmi_x1{0}, pmi_x2{0};
  std::atomic<int32_t>  ssb_rsrp{0}, csirs_rsrp{0};
  std::atomic<uint32_t> dl_mcs{0}, ul_mcs{0}, dl_mcs_table{0}, ul_mcs_table{0}, dl_max_mcs{0};
  std::atomic<uint32_t> dl_prb_last{0}, ul_prb_last{0}, dl_tbs_last{0}, ul_tbs_last{0};
  std::atomic<int32_t>  phr{0};
  std::atomic<uint32_t> bsr{0};
  std::atomic<uint32_t> frame{0}, slot{0};
  std::atomic<uint32_t> pusch_kos{0}, pucch_kos{0};
  std::atomic<bool>     ul_failure{false};
  std::atomic<uint64_t> ul_failure_since_ns{0};

  // Last DL buffer occupancy per logical channel.
  std::array<std::atomic<uint32_t>, max_lcids> dl_bo{};

  // Last decoded CSI report (Group 12 extension) and interval report counters. CSI reports arrive on the cell
  // executor, the publisher copies the snapshot: guarded by a mutex (uncontended in practice, ~1 report / 10-80 ms).
  std::mutex            csi_mutex;
  aurora_csi_ue_stats_t csi_snapshot{};
  std::atomic<uint32_t> csi_reports{0}, csi_invalid_reports{0};

  // DL HARQ consecutive NACK/DTX per HARQ process.
  std::array<std::atomic<uint32_t>, max_harq_ids> cons_nack{};

  // Publisher-owned cumulative counters (only the publisher touches them); a new UE requests their reset.
  uint32_t          dl_starvation_events = 0;
  uint32_t          ul_starvation_events = 0;
  std::atomic<bool> reset_starvation{false};
};

struct rlc_bearer_acc {
  std::atomic<bool>                                                                   active{false};
  std::atomic<uint32_t>                                                               owner{u32_max};
  std::atomic<uint32_t>                                                               mode{0};
  std::array<std::atomic<uint64_t>, static_cast<unsigned>(rlc_counter::nof_counters)> c{};
  std::array<std::atomic<uint32_t>, static_cast<unsigned>(rlc_gauge::nof_gauges)>     g{};
};

struct pdcp_bearer_acc {
  std::atomic<bool>                                                                    active{false};
  std::atomic<uint32_t>                                                                owner{u32_max};
  std::atomic<uint32_t>                                                                mode{0};
  std::array<std::atomic<uint64_t>, static_cast<unsigned>(pdcp_counter::nof_counters)> c{};
  std::atomic<uint64_t> latency_sum_ns{0}, sojourn_sum_ns{0};
  std::atomic<uint32_t> latency_count{0}, sojourn_count{0};
};

struct gtp_tunnel_acc {
  std::atomic<bool>                                                                   active{false};
  std::atomic<uint32_t>                                                               owner{u32_max};
  std::atomic<uint32_t>                                                               teid_local{0}, teid_peer{0};
  std::atomic<uint32_t>                                                               qfi{0};
  std::array<std::atomic<uint64_t>, static_cast<unsigned>(gtp_counter::nof_counters)> c{};
  std::atomic<uint64_t> latency_sum_ns{0};
  std::atomic<uint32_t> latency_count{0};
};

/// Handover executions of one source DU.
struct du_ho_acc {
  std::atomic<bool>     used{false};
  std::atomic<uint64_t> gnb_du_id{0};
  std::atomic<uint32_t> attempts{0}, success{0};             ///< Interval.
  std::atomic<uint32_t> attempts_total{0}, success_total{0}; ///< Cumulative.
};

struct global_state {
  config cfg;

  std::array<flow_acc, static_cast<unsigned>(flow::nof_flows)> flows;

  // Group 2/3.
  std::atomic<uint64_t> dl_harq_ack{0}, dl_harq_nack{0};
  std::atomic<uint32_t> dl_harq_max_cons{0};
  std::atomic<uint64_t> ul_crc_ok{0}, ul_crc_ko{0};

  // Group 5/6 (per grant).
  stat_acc pdsch_mcs, pdsch_prb, pdsch_tbs;
  stat_acc pusch_mcs, pusch_prb, pusch_tbs;
  hist_acc hist_dl_mcs, hist_dl_prb, hist_ul_mcs, hist_ul_prb;

  // Group 11 (cell health).
  std::atomic<uint32_t> nof_cell_prbs{0};
  std::atomic<uint32_t> cell_pci{0};
  std::atomic<uint32_t> sched_slots{0}, dl_slots{0}, ul_slots{0};
  std::atomic<uint64_t> sched_latency_sum_us{0}, sched_latency_max_us{0};
  std::atomic<uint32_t> ue_attach{0}, ue_detach{0};
  std::atomic<uint32_t> du_count{0};
  std::atomic<float>    rsrq_sum{0};
  std::atomic<uint32_t> rsrq_count{0};
  std::array<du_ho_acc, AURORA_MAX_HO_DUS> ho_dus;
  std::mutex                               ho_dus_mutex; ///< Serializes DU slot allocation (rare events).
  std::atomic<uint32_t>                    ho_prep_attempts{0}, ho_prep_success{0};
  std::atomic<uint32_t>                    ho_prep_attempts_total{0}, ho_prep_success_total{0};
  std::atomic<uint64_t>                 gtp_unknown_teid{0};

  f1c_acc f1c;
  f1u_acc f1u;

  std::array<mac_ue_acc, max_nof_ues>                                  mac_ues;
  /// Sink for per-UE updates of grants whose UE slot is not owned (never published).
  mac_ue_acc scratch_ue;
  std::array<std::array<rlc_bearer_acc, max_rbs_per_ue>, max_nof_ues>  rlc;
  std::array<std::array<pdcp_bearer_acc, max_rbs_per_ue>, max_nof_ues> pdcp;
  std::array<std::array<gtp_tunnel_acc, max_gtp_tunnels>, max_nof_ues> gtp;

  /// Group 17 working buffer: the thread collector fills it, the publisher copies it to shared memory and resets it.
  aurora_thread_stats_t thread_buf{};
};

/// Process-wide state, allocated by start() (release) and read by hooks (acquire).
extern std::atomic<global_state*> state;
/// Attached shared memory segment, set by start().
extern aurora_metrics* shm;

/// Drains all accumulators into `shm` (publisher thread only).
void publish_interval(aurora_metrics& out, global_state& st);
/// Initialises the shared memory content (static fields, min sentinels).
void init_shm_content(aurora_metrics& out, const config& cfg);

uint64_t monotonic_ns();

} // namespace detail
} // namespace aurora
} // namespace ocudu
