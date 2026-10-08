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
  return is_active() ? state.load(std::memory_order_acquire) : nullptr;
}

/// True if the accumulator slot is active and currently owned by this UE index (slots are shared modulo
/// max_nof_ues; late events of removed UEs must not resurrect or pollute a slot).
template <typename Acc>
bool owned_by(const Acc& acc, unsigned ue_index)
{
  return get(acc.active) && get(acc.owner) == ue_index;
}

mac_ue_acc* mac_ue(unsigned ue_index)
{
  global_state* s = st();
  if (s == nullptr) {
    return nullptr;
  }
  mac_ue_acc& u = s->mac_ues[ue_slot(ue_index)];
  return owned_by(u, ue_index) ? &u : nullptr;
}

/// Zeroes every per-interval counter and gauge of a MAC UE slot.
void reset_mac_ue(mac_ue_acc& u)
{
  for (auto* a : {&u.dl_tbs, &u.ul_tbs, &u.dl_sdu_bytes, &u.ul_sdu_bytes, &u.dl_prbs, &u.ul_prbs, &u.ul_failure_since_ns}) {
    set(*a, uint64_t{0});
  }
  for (auto* a : {&u.dl_sdus,       &u.ul_sdus,    &u.dl_retx_prbs, &u.ul_retx_prbs, &u.dl_harq_errors, &u.ul_harq_errors,
                  &u.ulsch_dtx,     &u.dl_ok,      &u.dl_nok,       &u.ul_ok,        &u.ul_nok,         &u.wb_cqi,
                  &u.ri,            &u.pmi_x1,     &u.pmi_x2,       &u.dl_mcs,       &u.ul_mcs,         &u.dl_mcs_table,
                  &u.ul_mcs_table,  &u.dl_max_mcs, &u.dl_prb_last,  &u.ul_prb_last,  &u.dl_tbs_last,    &u.ul_tbs_last,
                  &u.bsr,           &u.frame,      &u.slot,         &u.pusch_kos,    &u.pucch_kos}) {
    set(*a, 0U);
  }
  for (auto& r : u.dl_rounds) {
    set(r, 0U);
  }
  for (auto& r : u.ul_rounds) {
    set(r, 0U);
  }
  for (auto& c : u.cons_nack) {
    set(c, 0U);
  }
  for (auto& bo : u.dl_bo) {
    set(bo, 0U);
  }
  for (auto* a : {&u.ssb_rsrp, &u.csirs_rsrp, &u.phr}) {
    set(*a, int32_t{0});
  }
  for (auto* a : {&u.pusch_snr, &u.pucch_snr, &u.rsrp_dbm}) {
    set(*a, 0.0F);
  }
  set(u.ul_failure, false);
  set(u.dl_granted, false);
  set(u.ul_granted, false);
  set(u.csi_reports, 0U);
  set(u.csi_invalid_reports, 0U);
  // No CSI report received yet: every reportable quantity "not reported".
  aurora_csi_ue_stats_t none{};
  none.ri = none.li = none.wb_cqi_tb1 = none.wb_cqi_tb2 = AURORA_CSI_NOT_REPORTED;
  none.pmi_two_port = none.pmi_i11 = none.pmi_i12 = none.pmi_i13 = none.pmi_i2 = AURORA_CSI_NOT_REPORTED;
  std::lock_guard<std::mutex> lock(u.csi_mutex);
  u.csi_snapshot = none;
}

/// Refreshes the C-RNTI of an owned UE slot (it changes e.g. on reestablishment).
void touch_ue(mac_ue_acc& u, uint16_t rnti)
{
  if (rnti != 0) {
    set(u.rnti, rnti);
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
  // min/max first: a concurrent record() then lands in this interval's count with, at worst, its min/max in the
  // next interval, never with a count of zero and a sentinel min.
  uint32_t mn = take(min_val, u32_max);
  out.max_val = take(max_val);
  for (unsigned i = 0; i != nof_hist_bins; ++i) {
    out.bins[i] = take(bins[i]);
  }
  out.sum     = take(sum);
  out.count   = take(count);
  out.min_val = (out.count != 0 && mn != u32_max) ? mn : 0;
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
  set(u.active, false);
  reset_mac_ue(u);
  set(u.reset_starvation, true);
  set(u.rnti, rnti);
  set(u.owner, uint32_t(ue_index));
  u.active.store(true, std::memory_order_release);
  add(s->ue_attach, 1U);
}

void aurora::on_ue_removed(unsigned ue_index) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  unsigned slot = ue_slot(ue_index);
  if (owned_by(s->mac_ues[slot], ue_index)) {
    set(s->mac_ues[slot].active, false);
    set(s->mac_ues[slot].rnti, uint16_t{0});
  }
  for (auto& rb : s->rlc[slot]) {
    if (owned_by(rb, ue_index)) {
      set(rb.active, false);
    }
  }
  add(s->ue_detach, 1U);
}

static void on_grant(const grant_info& g, bool is_dl)
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  mac_ue_acc* up = mac_ue(g.ue_index);
  mac_ue_acc& u  = up != nullptr ? *up : s->scratch_ue;
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
  if (status == harq_ack_status::ack) {
    add(s->dl_harq_ack, uint64_t{1});
  } else {
    // OAI counts NACK and DTX together as "nack".
    add(s->dl_harq_nack, uint64_t{1});
  }
  mac_ue_acc* u = mac_ue(ue_index);
  if (u == nullptr) {
    return;
  }
  touch_ue(*u, rnti);
  std::atomic<uint32_t>& cons = u->cons_nack[harq_id % max_harq_ids];
  if (status == harq_ack_status::ack) {
    add(u->dl_ok, 1U);
    set(cons, 0U);
    return;
  }
  add(u->dl_nok, 1U);
  uint32_t n = cons.fetch_add(1, std::memory_order_relaxed) + 1;
  atomic_max(s->dl_harq_max_cons, n);
}

void aurora::on_ul_crc(unsigned ue_index, uint16_t rnti, bool crc_ok, std::optional<float> sinr_db) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return;
  }
  add(crc_ok ? s->ul_crc_ok : s->ul_crc_ko, uint64_t{1});
  mac_ue_acc* u = mac_ue(ue_index);
  if (u == nullptr) {
    return;
  }
  touch_ue(*u, rnti);
  add(crc_ok ? u->ul_ok : u->ul_nok, 1U);
  if (sinr_db.has_value() && std::isfinite(*sinr_db)) {
    set(u->pusch_snr, *sinr_db);
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
  add(u->csi_reports, 1U);
  if (!csi.valid) {
    add(u->csi_invalid_reports, 1U);
    return;
  }

  // OAI-layout fields (Group 12).
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
  std::optional<float> best = csi.l1_rsrp_best_dbm();
  if (best) {
    set(u->rsrp_dbm, *best);
    set(csi.rsrp_from_ssb ? u->ssb_rsrp : u->csirs_rsrp, int32_t(std::lround(*best)));
  }

  // Full report (OCUDU extension).
  constexpr uint8_t     nr = AURORA_CSI_NOT_REPORTED;
  aurora_csi_ue_stats_t c{};
  c.nof_rs        = uint8_t(std::min(csi.nof_rs, csi_info::max_rs));
  c.rsrp_from_ssb = csi.rsrp_from_ssb ? 1 : 0;
  for (unsigned i = 0; i != c.nof_rs; ++i) {
    c.cri[i]         = csi.cri[i];
    c.l1_rsrp_dbm[i] = csi.l1_rsrp_dbm[i];
  }
  c.l1_rsrp_best_dbm = best ? int16_t(std::lround(*best)) : 0;
  c.ri               = csi.ri ? uint8_t(*csi.ri) : nr;
  c.li               = csi.li ? uint8_t(*csi.li) : nr;
  c.wb_cqi_tb1       = csi.wb_cqi ? uint8_t(*csi.wb_cqi) : nr;
  c.wb_cqi_tb2       = csi.wb_cqi_tb2 ? uint8_t(*csi.wb_cqi_tb2) : nr;
  c.nof_subbands_tb1 = uint8_t(std::min(csi.nof_subbands_tb1, csi_info::max_subbands));
  c.nof_subbands_tb2 = uint8_t(std::min(csi.nof_subbands_tb2, csi_info::max_subbands));
  for (unsigned i = 0; i != c.nof_subbands_tb1; ++i) {
    c.sb_cqi_diff_tb1[i] = csi.sb_cqi_diff_tb1[i];
  }
  for (unsigned i = 0; i != c.nof_subbands_tb2; ++i) {
    c.sb_cqi_diff_tb2[i] = csi.sb_cqi_diff_tb2[i];
  }
  c.pmi_type      = static_cast<uint8_t>(csi.pmi);
  c.pmi_two_port  = csi.pmi == csi_info::pmi_kind::two_port ? uint8_t(csi.pmi_two_port) : nr;
  c.pmi_i11       = csi.pmi_i1 ? uint8_t(*csi.pmi_i1) : nr;
  c.pmi_i12       = csi.pmi_i12 ? uint8_t(*csi.pmi_i12) : nr;
  c.pmi_i13       = csi.pmi_i13 ? uint8_t(*csi.pmi_i13) : nr;
  c.pmi_i2        = csi.pmi_i2 ? uint8_t(*csi.pmi_i2) : nr;
  c.t2_i12        = uint16_t(csi.t2_i12);
  c.t2_nof_layers = uint8_t(std::min(csi.t2_nof_layers, csi_info::max_t2_layers));
  for (unsigned l = 0; l != c.t2_nof_layers; ++l) {
    c.t2_i13[l]        = csi.t2_i13[l];
    c.t2_nof_coeffs[l] = uint8_t(std::min<unsigned>(csi.t2_nof_coeffs[l], csi_info::max_t2_coeffs));
    for (unsigned k = 0; k != c.t2_nof_coeffs[l]; ++k) {
      c.t2_i14[l][k] = csi.t2_i14[l][k];
      c.t2_i21[l][k] = csi.t2_i21[l][k];
      c.t2_i22[l][k] = csi.t2_i22[l][k];
    }
  }
  std::lock_guard<std::mutex> lock(u->csi_mutex);
  u->csi_snapshot = c;
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

void aurora::on_dl_buffer_state(unsigned ue_index, unsigned lcid, uint32_t nof_bytes) noexcept
{
  mac_ue_acc* u = mac_ue(ue_index);
  if (u == nullptr) {
    return;
  }
  set(u->dl_bo[lcid % max_lcids], nof_bytes);
  uint64_t total = 0;
  for (const auto& bo : u->dl_bo) {
    total += get(bo);
  }
  record_flow(flow::mac_dl_bo, total);
}

void aurora::on_mac_dl_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  record_flow(flow::rlc_mac, nof_bytes);
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(u->dl_sdu_bytes, uint64_t{nof_bytes});
    add(u->dl_sdus, 1U);
  }
}

void aurora::on_mac_ul_sdu(unsigned ue_index, uint32_t nof_bytes) noexcept
{
  record_flow(flow::mac_rlc, nof_bytes);
  if (mac_ue_acc* u = mac_ue(ue_index)) {
    add(u->ul_sdu_bytes, uint64_t{nof_bytes});
    add(u->ul_sdus, 1U);
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

static du_ho_acc* du_ho(global_state& s, uint64_t du_id)
{
  std::lock_guard<std::mutex> lock(s.ho_dus_mutex);
  for (auto& d : s.ho_dus) {
    if (get(d.used) && get(d.gnb_du_id) == du_id) {
      return &d;
    }
  }
  for (auto& d : s.ho_dus) {
    if (!get(d.used)) {
      set(d.gnb_du_id, du_id);
      d.used.store(true, std::memory_order_release);
      return &d;
    }
  }
  return nullptr;
}

void aurora::on_handover_attempt(uint64_t source_gnb_du_id) noexcept
{
  if (global_state* s = st()) {
    if (du_ho_acc* d = du_ho(*s, source_gnb_du_id)) {
      add(d->attempts, 1U);
      add(d->attempts_total, 1U);
    }
  }
}

void aurora::on_handover_success(uint64_t source_gnb_du_id) noexcept
{
  if (global_state* s = st()) {
    if (du_ho_acc* d = du_ho(*s, source_gnb_du_id)) {
      add(d->success, 1U);
      add(d->success_total, 1U);
    }
  }
}

void aurora::on_handover_preparation_attempt() noexcept
{
  if (global_state* s = st()) {
    add(s->ho_prep_attempts, 1U);
    add(s->ho_prep_attempts_total, 1U);
  }
}

void aurora::on_handover_preparation_success() noexcept
{
  if (global_state* s = st()) {
    add(s->ho_prep_success, 1U);
    add(s->ho_prep_success_total, 1U);
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
  float cur = get(s->rsrq_sum);
  while (!s->rsrq_sum.compare_exchange_weak(cur, cur + rsrq_db, std::memory_order_relaxed)) {
  }
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
  set(rb.active, false);
  for (auto& c : rb.c) {
    set(c, uint64_t{0});
  }
  for (auto& g : rb.g) {
    set(g, 0U);
  }
  set(rb.mode, static_cast<uint32_t>(mode));
  set(rb.owner, uint32_t(ue_index));
  rb.active.store(true, std::memory_order_release);
}

static rlc_bearer_acc* rlc_bearer(unsigned ue_index, unsigned rb_index)
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue) {
    return nullptr;
  }
  rlc_bearer_acc& rb = s->rlc[ue_slot(ue_index)][rb_index];
  return owned_by(rb, ue_index) ? &rb : nullptr;
}

void aurora::rlc_add(unsigned ue_index, unsigned rb_index, rlc_counter c, uint64_t value) noexcept
{
  if (c >= rlc_counter::nof_counters) {
    return;
  }
  if (rlc_bearer_acc* rb = rlc_bearer(ue_index, rb_index)) {
    add(rb->c[static_cast<unsigned>(c)], value);
  }
}

void aurora::rlc_set(unsigned ue_index, unsigned rb_index, rlc_gauge g, uint32_t value) noexcept
{
  if (g >= rlc_gauge::nof_gauges) {
    return;
  }
  if (rlc_bearer_acc* rb = rlc_bearer(ue_index, rb_index)) {
    set(rb->g[static_cast<unsigned>(g)], value);
  }
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
  set(rb.active, false);
  for (auto& c : rb.c) {
    set(c, uint64_t{0});
  }
  set(rb.latency_sum_ns, uint64_t{0});
  set(rb.sojourn_sum_ns, uint64_t{0});
  set(rb.latency_count, 0U);
  set(rb.sojourn_count, 0U);
  set(rb.mode, static_cast<uint32_t>(mode));
  set(rb.owner, uint32_t(ue_index));
  rb.active.store(true, std::memory_order_release);
}

static pdcp_bearer_acc* pdcp_bearer(unsigned ue_index, unsigned rb_index)
{
  global_state* s = st();
  if (s == nullptr || rb_index >= max_rbs_per_ue) {
    return nullptr;
  }
  pdcp_bearer_acc& rb = s->pdcp[ue_slot(ue_index)][rb_index];
  return owned_by(rb, ue_index) ? &rb : nullptr;
}

void aurora::pdcp_bearer_removed(unsigned ue_index, unsigned rb_index) noexcept
{
  if (pdcp_bearer_acc* rb = pdcp_bearer(ue_index, rb_index)) {
    set(rb->active, false);
  }
}

void aurora::pdcp_add(unsigned ue_index, unsigned rb_index, pdcp_counter c, uint64_t value) noexcept
{
  if (c >= pdcp_counter::nof_counters) {
    return;
  }
  if (pdcp_bearer_acc* rb = pdcp_bearer(ue_index, rb_index)) {
    add(rb->c[static_cast<unsigned>(c)], value);
  }
}

void aurora::pdcp_tx_proc_latency(unsigned ue_index, unsigned rb_index, uint64_t proc_latency_ns) noexcept
{
  if (pdcp_bearer_acc* rb = pdcp_bearer(ue_index, rb_index)) {
    add(rb->latency_sum_ns, proc_latency_ns);
    add(rb->latency_count, 1U);
  }
}

void aurora::pdcp_tx_sojourn(unsigned ue_index, unsigned rb_index, uint64_t sojourn_ns) noexcept
{
  if (pdcp_bearer_acc* rb = pdcp_bearer(ue_index, rb_index)) {
    add(rb->sojourn_sum_ns, sojourn_ns);
    add(rb->sojourn_count, 1U);
  }
}

void aurora::pdcp_hook_anomaly() noexcept
{
  if (is_active()) {
    anomaly_module_hook_pdcp();
  }
}

// ---------------------------------------------------------------------------------------------------------------------
// GTP-U.

int aurora::gtp_tunnel_created(unsigned ue_index, uint32_t local_teid, uint32_t peer_teid) noexcept
{
  global_state* s = st();
  if (s == nullptr) {
    return -1;
  }
  for (unsigned i = 0; i != max_gtp_tunnels; ++i) {
    gtp_tunnel_acc& t        = s->gtp[ue_slot(ue_index)][i];
    bool            expected = false;
    // Claim a free slot; it becomes visible to hooks/publisher only once owner and identity are set.
    if (!t.active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      continue;
    }
    set(t.owner, u32_max);
    for (auto& c : t.c) {
      set(c, uint64_t{0});
    }
    set(t.latency_sum_ns, uint64_t{0});
    set(t.latency_count, 0U);
    set(t.teid_local, local_teid);
    set(t.teid_peer, peer_teid);
    set(t.qfi, 0U);
    t.owner.store(uint32_t(ue_index), std::memory_order_release);
    return static_cast<int>(i);
  }
  return -1;
}

static gtp_tunnel_acc* gtp_tunnel(unsigned ue_index, unsigned tunnel_index)
{
  global_state* s = st();
  if (s == nullptr || tunnel_index >= max_gtp_tunnels) {
    return nullptr;
  }
  gtp_tunnel_acc& t = s->gtp[ue_slot(ue_index)][tunnel_index];
  return owned_by(t, ue_index) ? &t : nullptr;
}

void aurora::gtp_tunnel_removed(unsigned ue_index, unsigned tunnel_index) noexcept
{
  if (gtp_tunnel_acc* t = gtp_tunnel(ue_index, tunnel_index)) {
    set(t->owner, u32_max);
    t->active.store(false, std::memory_order_release);
  }
}

void aurora::gtp_set_qfi(unsigned ue_index, unsigned tunnel_index, uint8_t qfi) noexcept
{
  if (gtp_tunnel_acc* t = gtp_tunnel(ue_index, tunnel_index)) {
    set(t->qfi, uint32_t(qfi));
  }
}

void aurora::gtp_add(unsigned ue_index, unsigned tunnel_index, gtp_counter c, uint64_t value) noexcept
{
  if (c >= gtp_counter::nof_counters) {
    return;
  }
  if (gtp_tunnel_acc* t = gtp_tunnel(ue_index, tunnel_index)) {
    add(t->c[static_cast<unsigned>(c)], value);
  }
}

void aurora::gtp_unknown_teid() noexcept
{
  if (global_state* s = st()) {
    add(s->gtp_unknown_teid, uint64_t{1});
  }
}

void aurora::gtp_tx_latency(unsigned ue_index, unsigned tunnel_index, uint64_t proc_latency_ns) noexcept
{
  if (gtp_tunnel_acc* t = gtp_tunnel(ue_index, tunnel_index)) {
    add(t->latency_sum_ns, proc_latency_ns);
    add(t->latency_count, 1U);
  }
}
