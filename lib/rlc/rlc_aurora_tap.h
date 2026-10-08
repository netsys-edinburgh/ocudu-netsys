#pragma once

#include "ocudu/aurora/aurora.h"
#include "ocudu/rlc/rlc_buffer_state.h"
#include <chrono>
#include "ocudu/ran/rb_id.h"
#include "ocudu/ran/rlc_mode.h"

namespace ocudu {

/// Identity of an RLC bearer inside the AURORA per-UE/per-bearer table (Group 13). Mirrors the RLC metrics
/// containers into AURORA independently of whether RLC metrics reporting is enabled.
class rlc_aurora_tap
{
public:
  rlc_aurora_tap() = default;
  rlc_aurora_tap(du_ue_index_t ue_index_, rb_id_t rb_id) :
    ue_index(ue_index_),
    rb_index(rb_id.is_srb() ? aurora::srb_rb_index(srb_id_to_uint(rb_id.get_srb_id()))
                            : aurora::drb_rb_index(static_cast<unsigned>(rb_id.get_drb_id())))
  {
  }

  void set_mode(rlc_mode mode) const
  {
    if (valid()) {
      aurora::rlc_bearer_created(ue_index, rb_index, to_aurora_mode(mode));
    }
  }
  void add(aurora::rlc_counter c, uint64_t value) const
  {
    if (valid()) {
      aurora::rlc_add(ue_index, rb_index, c, value);
    }
  }
  void set(aurora::rlc_gauge g, uint32_t value) const
  {
    if (valid()) {
      aurora::rlc_set(ue_index, rb_index, g, value);
    }
  }
  bool valid() const { return rb_index < aurora::max_rbs_per_ue and aurora::is_active(); }

  /// TX gauges sampled whenever the buffer state is computed: occupancy, head-of-line delay, ReTx queue depth.
  void report_tx_buffer(const rlc_buffer_state& bs, size_t retx_depth) const
  {
    if (not valid()) {
      return;
    }
    set(aurora::rlc_gauge::txbuf_occ_bytes, bs.pending_bytes);
    uint32_t hol_us = 0;
    if (bs.hol_toa.has_value()) {
      auto d = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - *bs.hol_toa);
      hol_us = d.count() > 0 ? static_cast<uint32_t>(d.count()) : 0;
    }
    set(aurora::rlc_gauge::txsdu_wt_us, hol_us);
    set(aurora::rlc_gauge::retransmit_list_depth, static_cast<uint32_t>(retx_depth));
  }

  /// Rate limiter for the RX reassembly-window scan (at most one scan per 10 ms per bearer).
  bool rx_report_due() const
  {
    if (not valid()) {
      return false;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last_rx_report < std::chrono::milliseconds{10}) {
      return false;
    }
    last_rx_report = now;
    return true;
  }

  /// Maximum number of SNs visited by one RX window scan.
  static constexpr uint32_t max_rx_scan = 4096;

private:
  static aurora::rlc_mode to_aurora_mode(rlc_mode mode)
  {
    switch (mode) {
      case rlc_mode::am:
        return aurora::rlc_mode::am;
      case rlc_mode::tm:
        return aurora::rlc_mode::tm;
      default:
        return aurora::rlc_mode::um;
    }
  }

  unsigned                                      ue_index = 0;
  unsigned                                      rb_index = aurora::max_rbs_per_ue;
  mutable std::chrono::steady_clock::time_point last_rx_report{};
};

} // namespace ocudu
