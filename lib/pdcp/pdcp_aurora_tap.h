#pragma once

#include "ocudu/aurora/aurora.h"
#include "ocudu/ran/pdcp/pdcp_rlc_mode.h"
#include "ocudu/ran/rb_id.h"

namespace ocudu {

/// Identity of a PDCP bearer inside the AURORA per-UE/per-bearer table (Group 14). Mirrors the PDCP metrics
/// containers into AURORA independently of whether PDCP metrics reporting is enabled.
class pdcp_aurora_tap
{
public:
  pdcp_aurora_tap() = default;
  pdcp_aurora_tap(uint32_t ue_index_, rb_id_t rb_id, pdcp_rlc_mode mode) :
    ue_index(ue_index_),
    rb_index(rb_id.is_srb() ? aurora::srb_rb_index(srb_id_to_uint(rb_id.get_srb_id()))
                            : aurora::drb_rb_index(static_cast<unsigned>(rb_id.get_drb_id())))
  {
    if (valid()) {
      aurora::pdcp_bearer_created(
          ue_index, rb_index, mode == pdcp_rlc_mode::am ? aurora::rlc_mode::am : aurora::rlc_mode::um);
    }
  }

  void add(aurora::pdcp_counter c, uint64_t value) const
  {
    if (valid()) {
      aurora::pdcp_add(ue_index, rb_index, c, value);
    }
  }
  void proc_latency(uint64_t ns) const
  {
    if (valid()) {
      aurora::pdcp_tx_proc_latency(ue_index, rb_index, ns);
    }
  }
  void sojourn(uint64_t ns) const
  {
    if (valid()) {
      aurora::pdcp_tx_sojourn(ue_index, rb_index, ns);
    }
  }
  void removed() const
  {
    if (valid()) {
      aurora::pdcp_bearer_removed(ue_index, rb_index);
    }
  }
  bool valid() const { return rb_index < aurora::max_rbs_per_ue and aurora::is_active(); }

private:
  unsigned ue_index = 0;
  unsigned rb_index = aurora::max_rbs_per_ue;
};

} // namespace ocudu
