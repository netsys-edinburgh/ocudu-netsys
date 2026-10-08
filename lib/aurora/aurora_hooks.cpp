// AURORA producer hooks: cheap relaxed-atomic updates of the private accumulators (see aurora_state.h).

#include "anomaly/anomaly_module.h"
#include "aurora_state.h"
#include <algorithm>
#include <cmath>

using namespace ocudu;
using namespace aurora;
using namespace aurora::detail;

namespace {

/// Bytes histogram bin index, identical to OAI aurora_get_hist_bin().
unsigned byte_hist_bin(uint32_t val)
{
  if (val < 64) {
    return 0;
  }
  // bin i >= 1 covers [2^(i+5), 2^(i+6)).
  unsigned bin = 0;
  for (uint32_t v = val >> 6; v != 0; v >>= 1) {
    ++bin;
  }
  return std::min(bin, nof_hist_bins - 1);
}

global_state* st()
{
  return is_active() ? state : nullptr;
}

mac_ue_acc* mac_ue(unsigned ue_index)
{
  global_state* s = st();
  return s != nullptr ? &s->mac_ues[ue_slot(ue_index)] : nullptr;
}

/// Marks the UE slot active. Used by grant hooks in case the creation hook was missed (e.g. AURORA started late).
void touch_ue(mac_ue_acc& u, uint16_t rnti)
{
  if (rnti != 0) {
    set(u.rnti, rnti);
  }
  if (!get(u.active)) {
    set(u.active, true);
  }
}

} // namespace

void detail::hist_acc::record(uint32_t val, int shift)
{
  unsigned bin = shift < 0 ? byte_hist_bin(val) : std::min<unsigned>(val >> shift, nof_hist_bins - 1);
  add(bins[bin], 1U);
  add(count, 1U);
  add(sum, uint64_t{val});
  atomic_min(min_val, val);
  atomic_max(max_val, val);
}

void detail::hist_acc::drain(aurora_hist_t& out)
{
  for (unsigned i = 0; i != nof_hist_bins; ++i) {
    out.bins[i] = take(bins[i]);
  }
  out.count   = take(count);
  out.sum     = take(sum);
  uint32_t mn = take(min_val, u32_max);
  out.min_val = out.count != 0 ? mn : 0;
  out.max_val = take(max_val);
}

// ---------------------------------------------------------------------------------------------------------------------
// Group 1.

void aurora::record_flow(flow f, uint64_t nof_bytes) noexcept
{
  global_state* s = st();
  if (s == nullptr || f >= flow::nof_flows) {
    return;
  }
  flow_acc& acc = s->flows[static_cast<unsigned>(f)];
  add(acc.bytes, nof_bytes);
  acc.hist.record(static_cast<uint32_t>(std::min<uint64_t>(nof_bytes, u32_max)));
}

// ---------------------------------------------------------------------------------------------------------------------
// MAC / scheduler.

void aurora::on_ue_created(unsigned ue_index, uint16_t rnti) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  mac_ue_acc& u = s->mac_ues[ue_slot(ue_index)];
  for (auto& c : u.cons_nack) {
    set(c, 0U);
  }
  u.dl_starvation_events = 0;
  u.ul_starvation_events = 0;
  set(u.ul_failure, false);
  set(u.rnti, rnti);
  set(u.active, true);
  add(s->ue_attach, 1U);
}

void aurora::on_ue_removed(unsigned ue_index) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  unsigned slot = ue_slot(ue_index);
  set(s->mac_ues[slot].active, false);
  set(s->mac_ues[slot].rnti, uint16_t{0});
  for (auto& rb : s->rlc[slot]) {
    set(rb.active, false);
  }
  add(s->ue_detach, 1U);
}

void aurora::on_cu_up_ue_removed(unsigned ue_index) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  unsigned slot = ue_slot(ue_index);
  for (auto& rb : s->pdcp[slot]) {
    set(rb.active, false);
  }
  for (auto& t : s->gtp[slot]) {
    set(t.active, false);
  }
}

static void on_grant(const grant_info& g, bool is_dl)
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  mac_ue_acc& u = s->mac_ues[ue_slot(g.ue_index)];
  touch_ue(u, g.rnti);
  set(u.frame, uint32_t(g.sfn));
  set(u.slot, uint32_t(g.slot_index));
  unsigned round = std::min(g.nof_retxs, max_harq_rounds - 1);
  if (is_dl) {
    s->pdsch_mcs.record(g.mcs);
    s->pdsch_prb.record(g.nof_prbs);
    s->pdsch_tbs.record(g.tbs_bytes);
    s->hist_dl_mcs.record(g.mcs, 2);
    s->hist_dl_prb.record(g.nof_prbs, 5);
    add(u.dl_tbs, uint64_t{g.tbs_bytes});
    add(u.dl_prbs, uint64_t{g.nof_prbs});
    if (g.nof_retxs > 0) {
      add(u.dl_retx_prbs, uint32_t(g.nof_prbs));
    }
    add(u.dl_rounds[round], 1U);
    set(u.dl_mcs, uint32_t(g.mcs));
    set(u.dl_mcs_table, uint32_t(g.mcs_table));
    set(u.dl_prb_last, uint32_t(g.nof_prbs));
    set(u.dl_tbs_last, uint32_t(g.tbs_bytes));
    set(u.dl_granted, true);
  } else {
    s->pusch_mcs.record(g.mcs);
    s->pusch_prb.record(g.nof_prbs);
    s->pusch_tbs.record(g.tbs_bytes);
    s->hist_ul_mcs.record(g.mcs, 2);
    s->hist_ul_prb.record(g.nof_prbs, 5);
    add(u.ul_tbs, uint64_t{g.tbs_bytes});
    add(u.ul_prbs, uint64_t{g.nof_prbs});
    if (g.nof_retxs > 0) {
      add(u.ul_retx_prbs, uint32_t(g.nof_prbs));
    }
    add(u.ul_rounds[round], 1U);
    set(u.ul_mcs, uint32_t(g.mcs));
    set(u.ul_mcs_table, uint32_t(g.mcs_table));
    set(u.ul_prb_last, uint32_t(g.nof_prbs));
    set(u.ul_tbs_last, uint32_t(g.tbs_bytes));
    set(u.ul_granted, true);
  }
}

void aurora::on_pdsch_grant(const grant_info& grant) noexcept
{
  on_grant(grant, true);
}

void aurora::on_pusch_grant(const grant_info& grant) noexcept
{
  on_grant(grant, false);
}

void aurora::on_dl_harq_feedback(unsigned ue_index, uint16_t rnti, unsigned harq_id, harq_ack_status status) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  mac_ue_acc& u = s->mac_ues[ue_slot(ue_index)];
  touch_ue(u, rnti);
  std::atomic<uint32_t>& cons = u.cons_nack[harq_id % max_harq_ids];
  if (status == harq_ack_status::ack) {
    add(s->dl_harq_ack, uint64_t{1});
    add(u.dl_ok, 1U);
    set(cons, 0U);
    return;
  }
  // OAI counts NACK and DTX together as "nack".
  add(s->dl_harq_nack, uint64_t{1});
  add(u.dl_nok, 1U);
  uint32_t n = cons.fetch_add(1, std::memory_order_relaxed) + 1;
  atomic_max(s->dl_harq_max_cons, n);
}

void aurora::on_ul_crc(unsigned ue_index, uint16_t rnti, bool crc_ok, std::optional<float> sinr_db) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  mac_ue_acc& u = s->mac_ues[ue_slot(ue_index)];
  touch_ue(u, rnti);
  if (crc_ok) {
    add(s->ul_crc_ok, uint64_t{1});
    add(u.ul_ok, 1U);
  } else {
    add(s->ul_crc_ko, uint64_t{1});
    add(u.ul_nok, 1U);
  }
  if (sinr_db.has_value() && std::isfinite(*sinr_db)) {
    set(u.pusch_snr, *sinr_db);
  }
}

void aurora::on_ul_dtx(unsigned ue_index) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(u->ulsch_dtx, 1U);
  }
}

void aurora::on_harq_max_retx_failure(unsigned ue_index, bool is_dl) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(is_dl ? u->dl_harq_errors : u->ul_harq_errors, 1U);
  }
}

void aurora::on_pucch_snr(unsigned ue_index, float snr_db) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index); u != nullptr && std::isfinite(snr_db)) {
    set(u->pucch_snr, snr_db);
  }
}

void aurora::on_csi_report(unsigned ue_index, const csi_info& csi) noexcept
{
  mac_ue_acc* u = mac_ue(ue_index);
  if (u == nullptr) {
    return;
  }
  if (csi.wb_cqi) {
    set(u->wb_cqi, uint32_t(*csi.wb_cqi));
  }
  if (csi.ri) {
    set(u->ri, uint32_t(*csi.ri));
  }
  if (csi.pmi_i1) {
    set(u->pmi_x1, uint32_t(*csi.pmi_i1));
  }
  if (csi.pmi_i2) {
    set(u->pmi_x2, uint32_t(*csi.pmi_i2));
  }
  if (csi.l1_rsrp_dbm && std::isfinite(*csi.l1_rsrp_dbm)) {
    set(u->rsrp_dbm, *csi.l1_rsrp_dbm);
    set(csi.rsrp_from_ssb ? u->ssb_rsrp : u->csirs_rsrp, int32_t(std::lround(*csi.l1_rsrp_dbm)));
  }
}

void aurora::on_bsr(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    set(u->bsr, nof_bytes);
  }
}

void aurora::on_phr(unsigned ue_index, int ph_db) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    set(u->phr, int32_t(ph_db));
  }
}

void aurora::on_dl_buffer_state(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  (void)ue_index;
  record_flow(flow::mac_dl_bo, nof_bytes);
}

void aurora::on_mac_dl_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(u->dl_sdu_bytes, uint64_t{nof_bytes});
    add(u->dl_sdus, 1U);
    record_flow(flow::rlc_mac, nof_bytes);
  }
}

void aurora::on_mac_ul_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(u->ul_sdu_bytes, uint64_t{nof_bytes});
    add(u->ul_sdus, 1U);
    record_flow(flow::mac_rlc, nof_bytes);
  }
}

void aurora::on_dl_max_mcs(unsigned ue_index, unsigned mcs) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    set(u->dl_max_mcs, uint32_t(mcs));
  }
}

void aurora::on_consecutive_ul_ko(unsigned ue_index, unsigned pusch_kos, unsigned pucch_kos) noexcept
{
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    set(u->pusch_kos, uint32_t(pusch_kos));
    set(u->pucch_kos, uint32_t(pucch_kos));
  }
}

void aurora::on_ul_failure(unsigned ue_index, bool declared) noexcept
{
  mac_ue_acc* u = mac_ue(ue_index);
  if (u == nullptr) {
    return;
  }
  if (declared && !get(u->ul_failure)) {
    set(u->ul_failure_since_ns, monotonic_ns());
  }
  set(u->ul_failure, declared);
}

void aurora::on_scheduler_slot(unsigned                  pci,
                               std::chrono::microseconds decision_latency,
                               unsigned                  nof_cell_prbs,
                               bool                      is_dl_slot,
                               bool                      is_ul_slot) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  anomaly_module_hook_mac();
  set(s->cell_pci, uint32_t(pci));
  set(s->nof_cell_prbs, uint32_t(nof_cell_prbs));
  add(s->sched_slots, 1U);
  if (is_dl_slot) {
    add(s->dl_slots, 1U);
  }
  if (is_ul_slot) {
    add(s->ul_slots, 1U);
  }
  auto lat = static_cast<uint64_t>(std::max<int64_t>(decision_latency.count(), 0));
  add(s->sched_latency_sum_us, lat);
  atomic_max(s->sched_latency_max_us, lat);
}

// ---------------------------------------------------------------------------------------------------------------------
// CU-CP.

static rc_cell_acc* rc_cell(global_state& s, uint32_t cell_id)
{
  for (auto& c : s.rc_cells) {
    if (get(c.used) && get(c.cell_id) == cell_id) {
      return &c;
    }
  }
  for (auto& c : s.rc_cells) {
    bool expected = false;
    if (c.used.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
      set(c.cell_id, cell_id);
      return &c;
    }
  }
  return nullptr;
}

void aurora::on_handover_attempt(uint32_t cell_id) noexcept
{
  if (global_state* s = st()) {
    if (rc_cell_acc* c = rc_cell(*s, cell_id)) {
      add(c->ho_attempts, 1U);
    }
  }
}

void aurora::on_handover_success(uint32_t cell_id) noexcept
{
  if (global_state* s = st()) {
    if (rc_cell_acc* c = rc_cell(*s, cell_id)) {
      add(c->ho_success, 1U);
    }
  }
}

void aurora::on_du_count(unsigned nof_dus) noexcept
{
  if (global_state* s = st()) {
    set(s->du_count, uint32_t(nof_dus));
  }
}

void aurora::on_serving_cell_rsrq(float rsrq_db) noexcept
{
  global_state* s = st();
  if (s == nullptr || !std::isfinite(rsrq_db)) {
    return;
  }
  // Single writer (CU-CP control executor); a plain load/store sum is sufficient.
  set(s->rsrq_sum, get(s->rsrq_sum) + rsrq_db);
  add(s->rsrq_count, 1U);
}

// ---------------------------------------------------------------------------------------------------------------------
// RLC.

void aurora::rlc_bearer_created(unsigned ue_index, unsigned rb_index, rlc_mode mode) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue) {
    return;
  }
  rlc_bearer_acc& rb = s->rlc[ue_slot(ue_index)][rb_index];
  for (auto& c : rb.c) {
    set(c, uint64_t{0});
  }
  for (auto& g : rb.g) {
    set(g, 0U);
  }
  set(rb.mode, static_cast<uint32_t>(mode));
  set(rb.active, true);
}

void aurora::rlc_add(unsigned ue_index, unsigned rb_index, rlc_counter c, uint64_t value) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue || c >= rlc_counter::nof_counters) {
    return;
  }
  rlc_bearer_acc& rb = s->rlc[ue_slot(ue_index)][rb_index];
  add(rb.c[static_cast<unsigned>(c)], value);
  if (!get(rb.active)) {
    set(rb.active, true);
  }
}

void aurora::rlc_set(unsigned ue_index, unsigned rb_index, rlc_gauge g, uint32_t value) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue || g >= rlc_gauge::nof_gauges) {
    return;
  }
  set(s->rlc[ue_slot(ue_index)][rb_index].g[static_cast<unsigned>(g)], value);
}

void aurora::rlc_hook_anomaly() noexcept
{
  if (is_active()) {
    anomaly_module_hook_rlc();
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// PDCP.

void aurora::pdcp_bearer_created(unsigned ue_index, unsigned rb_index, rlc_mode mode) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue) {
    return;
  }
  pdcp_bearer_acc& rb = s->pdcp[ue_slot(ue_index)][rb_index];
  for (auto& c : rb.c) {
    set(c, uint64_t{0});
  }
  set(rb.latency_sum_ns, uint64_t{0});
  set(rb.queue_wait_sum_ns, uint64_t{0});
  set(rb.latency_count, 0U);
  set(rb.mode, static_cast<uint32_t>(mode));
  set(rb.active, true);
}

void aurora::pdcp_add(unsigned ue_index, unsigned rb_index, pdcp_counter c, uint64_t value) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue || c >= pdcp_counter::nof_counters) {
    return;
  }
  pdcp_bearer_acc& rb = s->pdcp[ue_slot(ue_index)][rb_index];
  add(rb.c[static_cast<unsigned>(c)], value);
  if (!get(rb.active)) {
    set(rb.active, true);
  }
}

void aurora::pdcp_tx_latency(unsigned ue_index,
                             unsigned rb_index,
                             uint64_t proc_latency_ns,
                             uint64_t queue_wait_ns) noexcept
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue) {
    return;
  }
  pdcp_bearer_acc& rb = s->pdcp[ue_slot(ue_index)][rb_index];
  add(rb.latency_sum_ns, proc_latency_ns);
  add(rb.queue_wait_sum_ns, queue_wait_ns);
  add(rb.latency_count, 1U);
}

void aurora::pdcp_hook_anomaly() noexcept
{
  if (is_active()) {
    anomaly_module_hook_pdcp();
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// GTP-U.

void aurora::gtp_tunnel_created(unsigned ue_index,
                                unsigned tunnel_index,
                                uint32_t local_teid,
                                uint32_t peer_teid) noexcept
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels) {
    return;
  }
  gtp_tunnel_acc& t = s->gtp[ue_slot(ue_index)][tunnel_index];
  for (auto& c : t.c) {
    set(c, uint64_t{0});
  }
  set(t.latency_sum_ns, uint64_t{0});
  set(t.latency_count, 0U);
  set(t.teid_local, local_teid);
  set(t.teid_peer, peer_teid);
  set(t.qfi, 0U);
  set(t.active, true);
}

void aurora::gtp_tunnel_removed(unsigned ue_index, unsigned tunnel_index) noexcept
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels) {
    return;
  }
  set(s->gtp[ue_slot(ue_index)][tunnel_index].active, false);
}

void aurora::gtp_set_qfi(unsigned ue_index, unsigned tunnel_index, uint8_t qfi) noexcept
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels) {
    return;
  }
  set(s->gtp[ue_slot(ue_index)][tunnel_index].qfi, uint32_t(qfi));
}

void aurora::gtp_add(unsigned ue_index, unsigned tunnel_index, gtp_counter c, uint64_t value) noexcept
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels || c >= gtp_counter::nof_counters) {
    return;
  }
  add(s->gtp[ue_slot(ue_index)][tunnel_index].c[static_cast<unsigned>(c)], value);
}

void aurora::gtp_unknown_teid() noexcept
{
  if (global_state* s = st()) {
    add(s->gtp_unknown_teid, uint64_t{1});
  }
}

void aurora::gtp_tx_latency(unsigned ue_index, unsigned tunnel_index, uint64_t proc_latency_ns) noexcept
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels) {
    return;
  }
  gtp_tunnel_acc& t = s->gtp[ue_slot(ue_index)][tunnel_index];
  add(t.latency_sum_ns, proc_latency_ns);
  add(t.latency_count, 1U);
}
