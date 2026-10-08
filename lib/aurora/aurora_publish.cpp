// AURORA publisher: once per collection interval, drain the private accumulators into the shared-memory
// `aurora_metrics` and recompute the derived aggregates (OAI aurora_reset_thread() +
// aurora_recompute_ue_aggregates() + aurora_update_mac_ue_all_stats()).
//
// Semantics difference vs OAI: OAI producers wrote the shared segment live and a separate timer zeroed it, so a
// reader could observe a partially filled interval (and several fields raced the ~1 Hz dump_mac_stats() producer,
// see the comments in OAI aurora.c). Here the segment always holds the last *completed* interval and
// `timestamp_us` changes exactly once per interval.

#include "aurora_state.h"
#include <cstring>
#include <algorithm>
#include <ctime>
#include <sys/resource.h>

using namespace ocudu;
using namespace aurora;
using namespace aurora::detail;

uint64_t detail::monotonic_ns()
{
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ULL + uint64_t(ts.tv_nsec);
}

namespace {

constexpr int   mcs_hist_shift = 2;
constexpr int   prb_hist_shift = 5;
constexpr float no_samples     = 0.0F;

uint64_t& flow_size(aurora_metrics& m, flow f)
{
  switch (f) {
    case flow::bhtx_in:
      return m.bhtx_in_size;
    case flow::bhtx_out:
      return m.bhtx_out_size;
    case flow::bhrx_in:
      return m.bhrx_in_size;
    case flow::bhrx_out:
      return m.bhrx_out_size;
    case flow::mhtx_in:
      return m.mhtx_in_size;
    case flow::mhtx_out:
      return m.mhtx_out_size;
    case flow::mhrx_in:
      return m.mhrx_in_size;
    case flow::mhrx_out:
      return m.mhrx_out_size;
    case flow::f1u_rlc:
      return m.f1u_rlc_size;
    case flow::rlc_f1:
      return m.rlc_f1_size;
    case flow::rlc_mac:
      return m.rlc_mac_size;
    case flow::mac_rlc:
      return m.mac_rlc_size;
    default:
      return m.mac_dl_bo;
  }
}

aurora_hist_t& flow_hist(aurora_metrics& m, flow f)
{
  aurora_hist_t* h[] = {&m.hist_bhtx_in,
                        &m.hist_bhtx_out,
                        &m.hist_bhrx_in,
                        &m.hist_bhrx_out,
                        &m.hist_mhtx_in,
                        &m.hist_mhtx_out,
                        &m.hist_mhrx_in,
                        &m.hist_mhrx_out,
                        &m.hist_f1u_rlc,
                        &m.hist_rlc_f1,
                        &m.hist_rlc_mac,
                        &m.hist_mac_rlc,
                        &m.hist_mac_dl_bo};
  return *h[static_cast<unsigned>(f)];
}

void publish_flows(aurora_metrics& m, global_state& st)
{
  for (unsigned i = 0; i != static_cast<unsigned>(flow::nof_flows); ++i) {
    auto f            = static_cast<flow>(i);
    flow_size(m, f)   = take(st.flows[i].bytes);
    st.flows[i].hist.drain(flow_hist(m, f));
  }
}

void publish_harq_crc(aurora_metrics& m, global_state& st)
{
  m.dl_harq_ack       = take(st.dl_harq_ack);
  m.dl_harq_nack      = take(st.dl_harq_nack);
  m.dl_harq_total     = m.dl_harq_ack + m.dl_harq_nack;
  m.dl_harq_loss_rate = m.dl_harq_total != 0 ? double(m.dl_harq_nack) / double(m.dl_harq_total) : 0.0;
  m.dl_harq_max_cons  = take(st.dl_harq_max_cons);

  uint64_t ok        = take(st.ul_crc_ok);
  m.ul_crc_loss      = take(st.ul_crc_ko);
  m.ul_crc_total     = ok + m.ul_crc_loss;
  m.ul_crc_loss_rate = m.ul_crc_total != 0 ? double(m.ul_crc_loss) / double(m.ul_crc_total) : 0.0;
}

/// Drains a per-grant stat; returns (count, sum, min, max) with OAI "no sample" sentinels for min.
struct drained_stat {
  uint32_t count;
  uint64_t sum;
  uint32_t min_val;
  uint32_t max_val;
  float    avg() const { return count != 0 ? float(double(sum) / count) : no_samples; }
};

drained_stat drain(stat_acc& s)
{
  drained_stat d{};
  d.count   = take(s.count);
  d.sum     = take(s.sum);
  d.min_val = take(s.min_val, u32_max);
  d.max_val = take(s.max_val);
  return d;
}

void publish_fapi(aurora_metrics& m, global_state& st)
{
  drained_stat dmcs = drain(st.pdsch_mcs), dprb = drain(st.pdsch_prb), dtbs = drain(st.pdsch_tbs);
  drained_stat umcs = drain(st.pusch_mcs), uprb = drain(st.pusch_prb), utbs = drain(st.pusch_tbs);

  m.fapi_dl_total_pdsch_count = dtbs.count;
  m.fapi_dl_total_pdsch_size  = dtbs.sum;
  m.fapi_dl_avg_pdsch         = dtbs.avg();
  // OAI reset values when no PDU was scheduled: min = type max, max = 0, avg = 0.
  m.dl_fapi_mcs_min = dmcs.count != 0 ? uint8_t(dmcs.min_val) : 0xff;
  m.dl_fapi_mcs_max = uint8_t(dmcs.max_val);
  m.dl_fapi_mcs_avg = dmcs.avg();
  m.dl_fapi_prb_min = dprb.count != 0 ? dprb.min_val : u32_max;
  m.dl_fapi_prb_max = dprb.max_val;
  m.dl_fapi_prb_avg = dprb.avg();
  m.dl_fapi_tbs_min = dtbs.count != 0 ? dtbs.min_val : u32_max;
  m.dl_fapi_tbs_max = dtbs.max_val;
  m.dl_fapi_tbs_avg = m.fapi_dl_avg_pdsch;

  m.fapi_ul_total_pusch_count = utbs.count;
  m.fapi_ul_total_pusch_size  = utbs.sum;
  m.fapi_ul_avg_pusch         = utbs.avg();
  m.ul_fapi_mcs_min           = umcs.count != 0 ? uint8_t(umcs.min_val) : 0xff;
  m.ul_fapi_mcs_max           = uint8_t(umcs.max_val);
  m.ul_fapi_mcs_avg           = umcs.avg();
  m.ul_fapi_prb_min           = uprb.count != 0 ? uprb.min_val : u32_max;
  m.ul_fapi_prb_max           = uprb.max_val;
  m.ul_fapi_prb_avg           = uprb.avg();
  m.ul_fapi_tbs_min           = utbs.count != 0 ? utbs.min_val : u32_max;
  m.ul_fapi_tbs_max           = utbs.max_val;
  m.ul_fapi_tbs_avg           = m.fapi_ul_avg_pusch;

  st.hist_dl_mcs.drain(m.hist_dl_fapi_mcs);
  st.hist_dl_prb.drain(m.hist_dl_fapi_prb);
  st.hist_ul_mcs.drain(m.hist_ul_fapi_mcs);
  st.hist_ul_prb.drain(m.hist_ul_fapi_prb);
}

/// Totals gathered while filling the per-UE MAC table, consumed by the aggregate/KPM/RC step.
struct ue_totals {
  unsigned active_ues = 0;
  float    sinr_min = 1e38F, sinr_max = -1e38F, sinr_sum = 0;
  uint32_t csi_min = 255, csi_max = 0;
  float    csi_sum = 0;
  uint32_t bsr_min = u32_max, bsr_max = 0;
  float    bsr_sum   = 0;
  double   rsrp_sum  = 0;
  uint64_t dl_prbs   = 0, ul_prbs = 0;
  uint64_t dl_tbs    = 0, ul_tbs = 0;
  uint64_t ul_dtx    = 0, ul_harq = 0;
  uint32_t fapi_ues  = 0;
};

void publish_mac_ue(aurora_metrics& m, mac_ue_acc& u, unsigned slot, ue_totals& t, uint64_t now_ns, unsigned interval_ms)
{
  aurora_mac_ue_stats_t&    o = m.mac_ue_stats[slot];
  aurora_fapi_ue_metrics_t& f = m.fapi_ue_metrics[slot];
  std::memset(&f, 0, sizeof(f));

  if (!get(u.active)) {
    std::memset(&o, 0, sizeof(o));
    return;
  }
  uint16_t rnti = get(u.rnti);
  // The reader treats rnti == 0 as an unused slot; keep the slot visible even before the C-RNTI is known.
  o.rnti = rnti != 0 ? rnti : slot + 1;

  o.dl_aggr_tbs        = take(u.dl_tbs);
  o.ul_aggr_tbs        = take(u.ul_tbs);
  o.dl_curr_tbs        = o.dl_aggr_tbs;
  o.ul_curr_tbs        = o.ul_aggr_tbs;
  o.dl_aggr_bytes_sdus = take(u.dl_sdu_bytes);
  o.ul_aggr_bytes_sdus = take(u.ul_sdu_bytes);
  o.dl_sched_rb        = take(u.dl_prbs);
  o.ul_sched_rb        = take(u.ul_prbs);
  o.dl_aggr_prb        = uint32_t(o.dl_sched_rb);
  o.ul_aggr_prb        = uint32_t(o.ul_sched_rb);
  o.dl_aggr_sdus       = take(u.dl_sdus);
  o.ul_aggr_sdus       = take(u.ul_sdus);
  o.dl_aggr_retx_prb   = take(u.dl_retx_prbs);
  o.ul_aggr_retx_prb   = take(u.ul_retx_prbs);

  uint32_t dl_rounds_used = 0, ul_rounds_used = 0;
  for (unsigned r = 0; r != max_harq_rounds; ++r) {
    o.dl_harq_rounds[r] = take(u.dl_rounds[r]);
    o.ul_harq_rounds[r] = take(u.ul_rounds[r]);
    if (o.dl_harq_rounds[r] != 0) {
      dl_rounds_used = r + 1;
    }
    if (o.ul_harq_rounds[r] != 0) {
      ul_rounds_used = r + 1;
    }
    if (r < dl_harq_round_nb) {
      o.dl_harq[r] = o.dl_harq_rounds[r];
      o.ul_harq[r] = o.ul_harq_rounds[r];
    }
  }
  // OAI: number of HARQ rounds configured. OCUDU: highest round actually used this interval.
  o.dl_num_harq    = dl_rounds_used;
  o.ul_num_harq    = ul_rounds_used;
  o.dl_harq_errors = take(u.dl_harq_errors);
  o.ul_harq_errors = take(u.ul_harq_errors);
  o.ulsch_dtx      = take(u.ulsch_dtx);

  uint32_t dl_ok = take(u.dl_ok), dl_nok = take(u.dl_nok), ul_ok = take(u.ul_ok), ul_nok = take(u.ul_nok);
  o.dl_bler = (dl_ok + dl_nok) != 0 ? float(dl_nok) / float(dl_ok + dl_nok) : 0.0F;
  o.ul_bler = (ul_ok + ul_nok) != 0 ? float(ul_nok) / float(ul_ok + ul_nok) : 0.0F;

  o.pusch_snr       = get(u.pusch_snr);
  o.pucch_snr       = get(u.pucch_snr);
  o.rsrp_avg        = get(u.rsrp_dbm);
  o.wb_cqi          = uint8_t(get(u.wb_cqi));
  o.ri              = uint8_t(get(u.ri));
  o.pmi_x1          = uint8_t(get(u.pmi_x1));
  o.pmi_x2          = uint8_t(get(u.pmi_x2));
  o.ssb_rsrp_best   = int16_t(get(u.ssb_rsrp));
  o.csirs_rsrp_best = int16_t(get(u.csirs_rsrp));
  o.sinr_csi_x10    = 0; // L1-SINR CSI reporting not supported by OCUDU (see AURORA_PORT_NOT_POSSIBLE.md).
  o.dl_mcs1         = uint8_t(get(u.dl_mcs));
  o.ul_mcs1         = uint8_t(get(u.ul_mcs));
  o.dl_mcs2         = uint8_t(get(u.dl_mcs_table));
  o.ul_mcs2         = uint8_t(get(u.ul_mcs_table));
  o.dl_max_mcs      = uint8_t(get(u.dl_max_mcs));
  o.phr             = int8_t(get(u.phr));
  o.bsr             = get(u.bsr);
  o.frame           = uint16_t(get(u.frame));
  o.slot            = uint16_t(get(u.slot));
  o.pusch_consec_dtx = uint16_t(get(u.pusch_kos));
  o.pucch_consec_dtx = uint16_t(get(u.pucch_kos));
  o.ul_failure       = get(u.ul_failure) ? 1 : 0;
  // OAI: slots since UL failure declared. OCUDU: same quantity expressed in ms (numerology independent).
  o.ul_failure_timer = o.ul_failure != 0 ? uint32_t((now_ns - get(u.ul_failure_since_ns)) / 1000000ULL) : 0;

  // Starvation: intervals where the active UE got zero bytes scheduled (cumulative, as OAI).
  if (o.dl_curr_tbs == 0) {
    ++u.dl_starvation_events;
  }
  if (o.ul_curr_tbs == 0) {
    ++u.ul_starvation_events;
  }
  o.dl_starvation_events = u.dl_starvation_events;
  o.ul_starvation_events = u.ul_starvation_events;

  // Group 7: last scheduled DL/UL PDU of this interval.
  bool dl_g = take(u.dl_granted, false), ul_g = take(u.ul_granted, false);
  if (dl_g || ul_g) {
    f.rnti = uint16_t(o.rnti);
    ++t.fapi_ues;
    if (dl_g) {
      f.dl_mcs = uint8_t(get(u.dl_mcs));
      f.dl_prb = get(u.dl_prb_last);
      f.dl_tbs = get(u.dl_tbs_last);
    }
    if (ul_g) {
      f.ul_mcs = uint8_t(get(u.ul_mcs));
      f.ul_prb = get(u.ul_prb_last);
      f.ul_tbs = get(u.ul_tbs_last);
    }
  }

  ++t.active_ues;
  t.sinr_min = std::min(t.sinr_min, o.pusch_snr);
  t.sinr_max = std::max(t.sinr_max, o.pusch_snr);
  t.sinr_sum += o.pusch_snr;
  t.csi_min = std::min<uint32_t>(t.csi_min, o.wb_cqi);
  t.csi_max = std::max<uint32_t>(t.csi_max, o.wb_cqi);
  t.csi_sum += o.wb_cqi;
  t.bsr_min = std::min(t.bsr_min, o.bsr);
  t.bsr_max = std::max(t.bsr_max, o.bsr);
  t.bsr_sum += float(o.bsr);
  t.rsrp_sum += o.rsrp_avg;
  t.dl_prbs += o.dl_aggr_prb;
  t.ul_prbs += o.ul_aggr_prb;
  t.dl_tbs += o.dl_curr_tbs;
  t.ul_tbs += o.ul_curr_tbs;
  t.ul_dtx += o.ulsch_dtx;
  t.ul_harq += uint64_t(ul_ok) + ul_nok + o.ulsch_dtx;
  (void)interval_ms;
}

void publish_rlc(aurora_metrics& m, global_state& st)
{
  for (unsigned ue = 0; ue != max_nof_ues; ++ue) {
    uint16_t rnti     = get(st.mac_ues[ue].rnti);
    uint32_t ue_key   = rnti != 0 ? rnti : ue + 1;
    for (unsigned rb = 0; rb != max_rbs_per_ue; ++rb) {
      rlc_bearer_acc&                  a = st.rlc[ue][rb];
      aurora_rlc_radio_bearer_stats_t& o = m.rlc_rb_stats[ue][rb];
      if (!get(a.active)) {
        std::memset(&o, 0, sizeof(o));
        continue;
      }
      auto c = [&a](rlc_counter x) { return take(a.c[static_cast<unsigned>(x)]); };
      auto g = [&a](rlc_gauge x) { return get(a.g[static_cast<unsigned>(x)]); };
      o.rnti                   = ue_key;
      o.rbid                   = uint8_t(rb < first_drb_rb_index ? rb : rb - (first_drb_rb_index - 1));
      o.mode                   = get(a.mode);
      o.txpdu_pkts             = c(rlc_counter::txpdu_pkts);
      o.txpdu_bytes            = c(rlc_counter::txpdu_bytes);
      o.txsdu_pkts             = c(rlc_counter::txsdu_pkts);
      o.txsdu_bytes            = c(rlc_counter::txsdu_bytes);
      o.rxpdu_pkts             = c(rlc_counter::rxpdu_pkts);
      o.rxpdu_bytes            = c(rlc_counter::rxpdu_bytes);
      o.rxsdu_pkts             = c(rlc_counter::rxsdu_pkts);
      o.rxsdu_bytes            = c(rlc_counter::rxsdu_bytes);
      o.rxpdu_lost_pkts        = c(rlc_counter::rxpdu_lost_pkts);
      o.rxpdu_dd_pkts          = c(rlc_counter::rxpdu_dd_pkts);
      o.rxpdu_ow_pkts          = c(rlc_counter::rxpdu_ow_pkts);
      o.txpdu_retx_pkts        = uint32_t(c(rlc_counter::txpdu_retx_pkts));
      o.txpdu_retx_bytes       = uint32_t(c(rlc_counter::txpdu_retx_bytes));
      o.txpdu_dd_pkts          = uint32_t(c(rlc_counter::txpdu_dd_pkts));
      o.txpdu_dd_bytes         = uint32_t(c(rlc_counter::txpdu_dd_bytes));
      o.txpdu_segmented        = uint32_t(c(rlc_counter::txpdu_segmented));
      o.tx_window_stall_events = uint32_t(c(rlc_counter::tx_window_stall_events));
      o.txsdu_wt_us            = g(rlc_gauge::txsdu_wt_us);
      o.rxsdu_wt_us            = g(rlc_gauge::rxsdu_wt_us);
      o.txbuf_occ_bytes        = g(rlc_gauge::txbuf_occ_bytes);
      o.rxbuf_occ_bytes        = g(rlc_gauge::rxbuf_occ_bytes);
      o.retransmit_list_depth  = uint16_t(g(rlc_gauge::retransmit_list_depth));
    }
  }
}

void publish_pdcp(aurora_metrics& m, global_state& st, uint64_t& tx_sdu_bytes, uint64_t& rx_sdu_bytes)
{
  for (unsigned ue = 0; ue != max_nof_ues; ++ue) {
    for (unsigned rb = 0; rb != max_rbs_per_ue; ++rb) {
      pdcp_bearer_acc&                  a = st.pdcp[ue][rb];
      aurora_pdcp_radio_bearer_stats_t& o = m.pdcp_rb_stats[ue][rb];
      if (!get(a.active)) {
        std::memset(&o, 0, sizeof(o));
        continue;
      }
      auto c            = [&a](pdcp_counter x) { return take(a.c[static_cast<unsigned>(x)]); };
      o.rnti            = ue + 1; // CU-side UE key (no RNTI in CU-UP), see aurora_shm.h.
      o.rbid            = uint8_t(rb < first_drb_rb_index ? rb : rb - (first_drb_rb_index - 1));
      o.mode            = get(a.mode);
      o.txpdu_pkts      = c(pdcp_counter::txpdu_pkts);
      o.txpdu_bytes     = c(pdcp_counter::txpdu_bytes);
      o.txsdu_pkts      = c(pdcp_counter::txsdu_pkts);
      o.txsdu_bytes     = c(pdcp_counter::txsdu_bytes);
      o.rxpdu_pkts      = c(pdcp_counter::rxpdu_pkts);
      o.rxpdu_bytes     = c(pdcp_counter::rxpdu_bytes);
      o.rxsdu_pkts      = c(pdcp_counter::rxsdu_pkts);
      o.rxsdu_bytes     = c(pdcp_counter::rxsdu_bytes);
      o.rxpdu_lost_pkts = c(pdcp_counter::rxpdu_lost_pkts);
      o.rxpdu_dd_pkts   = c(pdcp_counter::rxpdu_dd_pkts);
      o.rxpdu_oo_pkts   = c(pdcp_counter::rxpdu_oo_pkts);
      uint32_t n           = take(a.latency_count);
      uint64_t lat         = take(a.latency_sum_ns);
      uint32_t ns          = take(a.sojourn_count);
      uint64_t soj         = take(a.sojourn_sum_ns);
      uint64_t avg_proc_ns = n != 0 ? lat / n : 0;
      uint64_t avg_soj_ns  = ns != 0 ? soj / ns : 0;
      o.tx_proc_latency_us = uint32_t(avg_proc_ns / 1000U);
      // OAI: always 0 (no PDCP TX queue). OCUDU queues SDUs in the crypto / UE executors.
      o.txsdu_wt_us = avg_soj_ns > avg_proc_ns ? uint32_t((avg_soj_ns - avg_proc_ns) / 1000U) : 0;
      tx_sdu_bytes += o.txsdu_bytes;
      rx_sdu_bytes += o.rxsdu_bytes;
    }
  }
}

void publish_gtp(aurora_metrics& m, global_state& st)
{
  uint64_t unknown = take(st.gtp_unknown_teid);
  for (unsigned ue = 0; ue != max_nof_ues; ++ue) {
    for (unsigned t = 0; t != max_gtp_tunnels; ++t) {
      gtp_tunnel_acc&            a = st.gtp[ue][t];
      aurora_gtp_tunnel_stats_t& o = m.gtp_tunnel_stats[ue][t];
      if (!get(a.active)) {
        std::memset(&o, 0, sizeof(o));
        continue;
      }
      auto c        = [&a](gtp_counter x) { return take(a.c[static_cast<unsigned>(x)]); };
      o.rnti        = ue + 1;
      o.teidgnb     = get(a.teid_local);
      o.teidupf     = get(a.teid_peer);
      o.qfi         = uint8_t(get(a.qfi));
      o.tx_pkts     = c(gtp_counter::tx_pkts);
      o.tx_bytes    = c(gtp_counter::tx_bytes);
      o.tx_gpdupkts = o.tx_pkts;
      o.tx_err_pkts = c(gtp_counter::tx_err_pkts);
      o.rx_pkts     = c(gtp_counter::rx_pkts);
      o.rx_bytes    = c(gtp_counter::rx_bytes);
      o.rx_gpdupkts = o.rx_pkts;
      o.rx_err_pkts = c(gtp_counter::rx_err_pkts);
      // Unknown-TEID packets cannot be attributed to a UE; reported on the first active tunnel.
      o.rx_unkwn_teid_pkts = c(gtp_counter::rx_unkwn_teid_pkts) + unknown;
      unknown              = 0;
      uint32_t n           = take(a.latency_count);
      uint64_t lat         = take(a.latency_sum_ns);
      o.tx_proc_latency_us = n != 0 ? uint32_t(lat / n / 1000U) : 0;
      // Reserved in OAI as well: no peer timestamps / no per-tunnel queue.
      o.delay_us     = 0;
      o.jitter_us    = 0;
      o.tx_buf_occ   = 0;
      o.rx_buf_occ   = 0;
      o.tx_loss_rate = (o.tx_pkts + o.tx_err_pkts) != 0 ? float(o.tx_err_pkts) / float(o.tx_pkts + o.tx_err_pkts) : 0;
      o.rx_loss_rate = (o.rx_pkts + o.rx_err_pkts) != 0 ? float(o.rx_err_pkts) / float(o.rx_pkts + o.rx_err_pkts) : 0;
    }
  }
}

void set_kpm(aurora_kpm_metric_t& k, uint32_t id, const char* name, bool is_real, uint64_t ival, double rval)
{
  k.measurement_id = id;
  std::strncpy(k.name, name, sizeof(k.name) - 1);
  k.name[sizeof(k.name) - 1] = '\0';
  k.value_type               = is_real ? 1 : 0;
  k.value                    = ival;
  k.value_real               = rval;
}

void publish_aggregates(aurora_metrics& m,
                        global_state&   st,
                        const ue_totals& t,
                        uint64_t        pdcp_tx_sdu_bytes,
                        uint64_t        pdcp_rx_sdu_bytes)
{
  const unsigned n = t.active_ues;
  m.active_ue_count = n;
  m.sinr_min        = n != 0 ? t.sinr_min : 0;
  m.sinr_max        = n != 0 ? t.sinr_max : 0;
  m.sinr_avg        = n != 0 ? t.sinr_sum / n : 0;
  m.csi_min         = n != 0 ? uint8_t(t.csi_min) : 0;
  m.csi_max         = n != 0 ? uint8_t(t.csi_max) : 0;
  m.csi_avg         = n != 0 ? t.csi_sum / n : 0;
  m.bsr_min         = n != 0 ? t.bsr_min : 0;
  m.bsr_max         = n != 0 ? t.bsr_max : 0;
  m.bsr_avg         = n != 0 ? t.bsr_sum / n : 0;
  m.fapi_ue_count   = t.fapi_ues;

  // Cell health. OAI hardcoded a 106-PRB carrier; OCUDU reports the real cell bandwidth and slot counts.
  uint32_t nof_prbs  = get(st.nof_cell_prbs);
  uint32_t slots     = take(st.sched_slots);
  uint32_t dl_slots  = take(st.dl_slots);
  uint32_t ul_slots  = take(st.ul_slots);
  uint64_t lat_sum   = take(st.sched_latency_sum_us);
  m.sched_loop_avg_us = slots != 0 ? float(double(lat_sum) / slots) : 0;
  m.sched_loop_max_us = take(st.sched_latency_max_us);
  m.dl_prb_utilization_pct =
      (nof_prbs != 0 && dl_slots != 0) ? float(double(t.dl_prbs) * 100.0 / (double(nof_prbs) * dl_slots)) : 0;
  m.ul_prb_utilization_pct =
      (nof_prbs != 0 && ul_slots != 0) ? float(double(t.ul_prbs) * 100.0 / (double(nof_prbs) * ul_slots)) : 0;
  m.f1u_dl_loss_rate = (m.f1u_rlc_size > m.rlc_mac_size && m.f1u_rlc_size > 0)
                           ? float(double(m.f1u_rlc_size - m.rlc_mac_size) / double(m.f1u_rlc_size))
                           : 0;
  m.ue_attach_count = take(st.ue_attach);
  m.ue_detach_count = take(st.ue_detach);
  m.du_count        = get(st.du_count);

  // Group 8: KPM.
  double sinr_avg = n != 0 ? double(t.sinr_sum) / n : 0.0;
  double rsrp_avg = n != 0 ? t.rsrp_sum / n : 0.0;
  uint64_t rlc_dd = 0, rlc_tx = 0;
  for (unsigned ue = 0; ue != max_nof_ues; ++ue) {
    for (unsigned rb = first_drb_rb_index; rb != max_rbs_per_ue; ++rb) {
      rlc_dd += m.rlc_rb_stats[ue][rb].txpdu_dd_pkts;
      rlc_tx += m.rlc_rb_stats[ue][rb].txsdu_pkts;
    }
  }
  aurora_kpm_metric_t* k = m.kpm_metrics;
  set_kpm(k[0], 1, "RRC.ConnNumber", false, n, 0);
  set_kpm(k[1], 2, "DRB.UEThpDl", true, 0, n != 0 ? double(t.dl_tbs) / n : 0);
  set_kpm(k[2], 3, "DRB.UEThpUl", true, 0, n != 0 ? double(t.ul_tbs) / n : 0);
  set_kpm(k[3], 4, "CARR.PDSCHMCSSched", true, 0, m.dl_fapi_mcs_avg);
  set_kpm(k[4], 5, "CARR.PUSCHMCSSched", true, 0, m.ul_fapi_mcs_avg);
  set_kpm(k[5], 6, "CARR.PRBUsageDl", false, t.dl_prbs, 0);
  set_kpm(k[6], 7, "CARR.PRBUsageUl", false, t.ul_prbs, 0);
  set_kpm(k[7], 8, "CARR.AverageCQI", true, 0, n != 0 ? double(t.csi_sum) / n : 0);
  set_kpm(k[8], 9, "DRB.PdcpSduBitrateDl", false, pdcp_tx_sdu_bytes, 0);
  set_kpm(k[9], 10, "DRB.PdcpSduBitrateUl", false, pdcp_rx_sdu_bytes, 0);
  set_kpm(k[10], 11, "L1M.UL-sinrAvg", true, 0, sinr_avg);
  set_kpm(k[11], 12, "L1M.DL-rsrpAvg", true, 0, rsrp_avg);
  set_kpm(k[12], 13, "DRB.RlcPacketDropRateDl", true, 0, rlc_tx != 0 ? double(rlc_dd) / double(rlc_tx) : 0);
  set_kpm(k[13], 14, "RRU.PrbTotDl", false, t.dl_prbs, 0);
  set_kpm(k[14], 15, "RRU.PrbTotUl", false, t.ul_prbs, 0);
  set_kpm(k[15], 16, "L1M.UL-dtxRatio", true, 0, t.ul_harq != 0 ? double(t.ul_dtx) / double(t.ul_harq) : 0);
  m.kpm_metric_count = 16;

  // Group 11: RC cells. Cell 0 = serving cell (MAC-derived), handover counters per cell.
  uint32_t rsrq_n   = take(st.rsrq_count);
  float    rsrq_sum = take(st.rsrq_sum, 0.0F);
  unsigned nof_cells = 0;
  for (unsigned i = 0; i != max_rc_cells; ++i) {
    if (get(st.rc_cells[i].used)) {
      nof_cells = i + 1;
    }
  }
  bool has_mac = (m.ran_type == RAN_GNB || m.ran_type == RAN_DU);
  if (has_mac && nof_cells == 0) {
    nof_cells = 1;
  }
  m.rc_cell_count = nof_cells;
  for (unsigned i = 0; i != max_rc_cells; ++i) {
    aurora_rc_stats_t& rc = m.rc_metrics[i];
    rc_cell_acc&       a  = st.rc_cells[i];
    rc.cell_id            = get(a.used) ? get(a.cell_id) : (i == 0 ? get(st.cell_pci) : 0);
    rc.handover_attempts  = get(a.ho_attempts);
    rc.handover_success   = get(a.ho_success);
    rc.handover_success_rate =
        rc.handover_attempts != 0 ? double(rc.handover_success) / double(rc.handover_attempts) : 0.0;
    if (i == 0) {
      rc.rsrp_avg          = rsrp_avg;
      rc.rsrq_avg          = rsrq_n != 0 ? double(rsrq_sum) / rsrq_n : 0.0;
      rc.sinr_avg          = sinr_avg;
      rc.cell_load_percent = uint32_t(std::min(100.0F, m.dl_prb_utilization_pct));
      rc.interference_level = sinr_avg >= 20.0 ? 0 : (sinr_avg >= 5.0 ? 1 : 2);
    }
  }
}

void publish_rusage(aurora_metrics& m)
{
  static rusage prev{};
  rusage        cur{};
  if (::getrusage(RUSAGE_SELF, &cur) != 0) {
    return;
  }
  auto us = [](const timeval& tv) { return uint64_t(tv.tv_sec) * 1000000ULL + uint64_t(tv.tv_usec); };
  m.ru_utime_us  = us(cur.ru_utime) - us(prev.ru_utime);
  m.ru_stime_us  = us(cur.ru_stime) - us(prev.ru_stime);
  m.ru_maxrss_kb = uint64_t(cur.ru_maxrss);
  m.ru_minflt    = uint64_t(cur.ru_minflt - prev.ru_minflt);
  m.ru_majflt    = uint64_t(cur.ru_majflt - prev.ru_majflt);
  m.ru_inblock   = uint64_t(cur.ru_inblock - prev.ru_inblock);
  m.ru_oublock   = uint64_t(cur.ru_oublock - prev.ru_oublock);
  m.ru_nvcsw     = uint64_t(cur.ru_nvcsw - prev.ru_nvcsw);
  m.ru_nivcsw    = uint64_t(cur.ru_nivcsw - prev.ru_nivcsw);
  prev           = cur;
}

} // namespace

void detail::init_shm_content(aurora_metrics& out, const config& cfg)
{
  std::memset(&out, 0, sizeof(out));
  out.ran_type               = static_cast<ran_type_t>(cfg.type);
  out.collection_interval_ms = uint32_t(cfg.collection_interval.count());
  out.dl_fapi_mcs_min        = 0xff;
  out.ul_fapi_mcs_min        = 0xff;
  out.dl_fapi_prb_min = out.ul_fapi_prb_min = u32_max;
  out.dl_fapi_tbs_min = out.ul_fapi_tbs_min = u32_max;
  out.timestamp_us                          = aurora_get_current_timestamp_us();
}

void detail::publish_interval(aurora_metrics& m, global_state& st)
{
  const uint64_t now_ns = monotonic_ns();

  publish_flows(m, st);
  publish_harq_crc(m, st);
  publish_fapi(m, st);

  ue_totals t;
  for (unsigned slot = 0; slot != max_nof_ues; ++slot) {
    publish_mac_ue(m, st.mac_ues[slot], slot, t, now_ns, unsigned(st.cfg.collection_interval.count()));
  }
  publish_rlc(m, st);
  uint64_t pdcp_tx = 0, pdcp_rx = 0;
  publish_pdcp(m, st, pdcp_tx, pdcp_rx);
  publish_gtp(m, st);
  publish_aggregates(m, st, t, pdcp_tx, pdcp_rx);
  publish_rusage(m);

  if (st.cfg.thread_metrics_enabled) {
    aurora_thread_reset_interval_stats(&m.thread_stats);
  }
  m.timestamp_us = aurora_get_current_timestamp_us();
}
