// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "static_sched_validator.h"
#include "ocudu/adt/expected.h"
#include "ocudu/adt/format.h"
#include "ocudu/ran/band_helper.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/ssb/ssb_mapping.h"
#include "ocudu/scheduler/sched_consts.h"
#include "ocudu/support/enum_utils.h"
#include "ocudu/support/math/math_utils.h"
#include <bitset>
#include <numeric>
#include <optional>

using namespace ocudu;

namespace {

/// \brief Identifies the origin of a \c periodic_occasion.
struct occasion_origin {
  enum class signal_type : uint8_t { SSB, CSI_RS, PRS } type;
  uint8_t primary_id;
  uint8_t secondary_id;
};

/// \brief A resource occupancy that recurs periodically in time.
struct periodic_occasion {
  /// Identifies the static resource that this occasion originates from.
  occasion_origin origin;
  /// Periodicity, in slots, at which this occasion recurs.
  unsigned slot_period;
  /// Slot, within \c slot_period, at which this occasion occurs.
  unsigned slot_offset;
  /// CRBs occupied by this occasion.
  crb_interval crbs;
  /// \brief RE mask per OFDM symbol.
  ///
  /// \remark Assumes the RE mask is the same for all CRBs.
  std::array<std::bitset<NOF_SUBCARRIERS_PER_RB>, NOF_OFDM_SYM_PER_SLOT_NORMAL_CP> re_masks{};

  /// Checks whether this and \c other occasion can ever fall on the same slot.
  bool collides_in_slots(const periodic_occasion& other) const
  {
    return crt_solvable(slot_offset, slot_period, other.slot_offset, other.slot_period);
  }

  /// Checks whether this and \c other occasion collide in the resource grid.
  bool collides_in_res(const periodic_occasion& other) const
  {
    if (not crbs.overlaps(other.crbs)) {
      return false;
    }

    for (unsigned sym = 0; sym != NOF_OFDM_SYM_PER_SLOT_NORMAL_CP; ++sym) {
      if ((re_masks[sym] & other.re_masks[sym]).any()) {
        return true;
      }
    }
    return false;
  }
};

/// \brief Description of a collision between two periodic occasions.
struct occasion_collision {
  /// Origins of the two colliding occasions.
  occasion_origin origin_a, origin_b;
  /// First absolute slot, modulo \c period, at which both occasions occur.
  unsigned slot;
  /// Period at which the collision occurs, in slots.
  unsigned period;
};

/// Returns the details of a collision between \c a and \c b, if one exists.
std::optional<occasion_collision> find_collision(const periodic_occasion& a, const periodic_occasion& b)
{
  if (not(a.collides_in_slots(b) and a.collides_in_res(b))) {
    return std::nullopt;
  }

  return occasion_collision{
      .origin_a = a.origin,
      .origin_b = b.origin,
      .slot     = crt(a.slot_offset, a.slot_period, b.slot_offset, b.slot_period),
      .period   = std::lcm(a.slot_period, b.slot_period),
  };
}

/// \brief Returns a human-readable identification of the static resource an occasion originates from.
std::string to_string(const occasion_origin& origin)
{
  switch (origin.type) {
    case occasion_origin::signal_type::SSB:
      return fmt::format("SSB occasion (ssb-Index={})", origin.primary_id);
    case occasion_origin::signal_type::CSI_RS:
      return fmt::format("NZP-CSI-RS resource (nzp-CSI-RS-ResourceId={})", origin.primary_id);
    case occasion_origin::signal_type::PRS:
      return fmt::format(
          "DL-PRS resource (PRS Resource Set Id={}, PRS Resource Id={})", origin.primary_id, origin.secondary_id);
  }
  ocudu_assert(false, "Invalid occasion origin signal type");
  return {};
}

/// \brief Builds the periodic occasions of the SSB burst of a cell.
///
/// One occasion is created per transmitted SSB candidate. All occasions share the same slot period
/// (the SSB burst set periodicity) and CRBs, but each has its own slot offset and OFDM symbols within that slot, as
/// per TS 38.213, Section 4.1.
std::vector<periodic_occasion> get_ssb_occasions(const ran_cell_config& ran)
{
  std::vector<periodic_occasion> occasions;

  const ssb_configuration& ssb_cfg    = ran.ssb_cfg;
  const subcarrier_spacing scs_common = ran.dl_cfg_common.init_dl_bwp.generic_params.scs;
  const ssb_pattern_case   ssb_case   = band_helper::get_ssb_pattern(ran.dl_carrier.band, ssb_cfg.scs);
  const crb_interval       ssb_crbs   = get_ssb_crbs(ssb_cfg.scs, scs_common, ssb_cfg.offset_to_point_A, ssb_cfg.k_ssb);
  const unsigned ssb_period_slots     = to_underlying(ssb_cfg.ssb_period) * get_nof_slots_per_subframe(scs_common);

  for (uint8_t ssb_idx : ssb_cfg.ssb_beams.transmitted_indexes()) {
    // Absolute OFDM symbol index of the SSB occasion within the SSB burst set period.
    const unsigned burst_symbol = ssb_get_l_first(ssb_case, ssb_idx);

    periodic_occasion occ;
    occ.origin      = {occasion_origin::signal_type::SSB, ssb_idx, 0};
    occ.slot_period = ssb_period_slots;
    occ.slot_offset = burst_symbol / NOF_OFDM_SYM_PER_SLOT_NORMAL_CP;
    occ.crbs        = ssb_crbs;

    // The 4 OFDM symbols of an SSB occasion never cross a slot boundary. The SSB fully occupies every RE of its
    // CRBs in those symbols.
    const unsigned symbol_start = burst_symbol % NOF_OFDM_SYM_PER_SLOT_NORMAL_CP;
    for (unsigned sym = symbol_start, sym_end = symbol_start + NOF_SSB_OFDM_SYMBOLS; sym != sym_end; ++sym) {
      occ.re_masks[sym].set();
    }

    occasions.push_back(occ);
  }

  return occasions;
}

} // namespace

error_type<std::string> ocudu::check_static_resource_collisions(const ran_cell_config& ran)
{
  // TODO: convert CSI-RS and PRS into periodic occasions as well.
  std::vector<periodic_occasion> occasions = get_ssb_occasions(ran);

  // Check every pair of occasions for a collision.
  for (auto it = occasions.begin(); it != occasions.end(); ++it) {
    auto it2 = it;
    for (++it2; it2 != occasions.end(); ++it2) {
      if (const std::optional<occasion_collision> coll = find_collision(*it, *it2)) {
        return make_unexpected(fmt::format("Detected a collision between the {} and the {} in slot {} (period {} "
                                           "slots)",
                                           to_string(coll->origin_a),
                                           to_string(coll->origin_b),
                                           coll->slot,
                                           coll->period));
      }
    }
  }
  return default_success_t();
}
