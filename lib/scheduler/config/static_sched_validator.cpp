// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/scheduler/config/static_sched_validator.h"
#include "ocudu/adt/expected.h"
#include "ocudu/adt/format.h"
#include "ocudu/ran/band_helper.h"
#include "ocudu/ran/csi_rs/csi_meas_config.h"
#include "ocudu/ran/csi_rs/csi_rs_config_helpers.h"
#include "ocudu/ran/csi_rs/csi_rs_pattern.h"
#include "ocudu/ran/csi_rs/frequency_allocation_type.h"
#include "ocudu/ran/prs/prs.h"
#include "ocudu/ran/resource_allocation/rb_interval.h"
#include "ocudu/ran/ssb/ssb_mapping.h"
#include "ocudu/scheduler/config/serving_cell_config_factory.h"
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
  enum class signal_type : uint8_t { SSB, NZP_CSI_RS, CSI_IM, PRS } type;
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
    case occasion_origin::signal_type::NZP_CSI_RS:
      return fmt::format("NZP-CSI-RS resource (nzp-CSI-RS-ResourceId={})", origin.primary_id);
    case occasion_origin::signal_type::CSI_IM:
      return fmt::format("CSI-IM resource (csi-IM-ResourceId={})", origin.primary_id);
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

/// \brief Builds the periodic occasions of the cell's periodic NZP-CSI-RS resources.
///
/// \remark ZP-CSI-RS resources are deliberately excluded: they do not represent a transmission by the cell, but an
/// instruction for the UE not to expect PDSCH there, so they cannot collide with anything.
std::vector<periodic_occasion> get_nzp_csi_rs_occasions(const serving_cell_config& serv_cell_cfg)
{
  std::vector<periodic_occasion> occasions;

  if (not serv_cell_cfg.csi_meas_cfg.has_value()) {
    return occasions;
  }

  for (const nzp_csi_rs_resource& res : serv_cell_cfg.csi_meas_cfg->nzp_csi_rs_res_list) {
    if (not res.csi_res_offset.has_value() or not res.csi_res_period.has_value()) {
      // Aperiodic NZP-CSI-RS resources do not recur, so they cannot be modelled as periodic occasions.
      continue;
    }

    const csi_rs_resource_mapping& res_mapping = res.res_mapping;
    const unsigned                 row         = csi_rs::get_csi_rs_resource_mapping_row_number(
        res_mapping.nof_ports, res_mapping.freq_density, res_mapping.cdm, res_mapping.fd_alloc);

    csi_rs_pattern_configuration pattern_cfg{
        .start_rb                 = res_mapping.freq_band_rbs.start(),
        .nof_rb                   = res_mapping.freq_band_rbs.length(),
        .csi_rs_mapping_table_row = row,
        .symbol_l0                = res_mapping.first_ofdm_symbol_in_td,
        .symbol_l1                = res_mapping.first_ofdm_symbol_in_td2.value_or(0),
        .cdm                      = res_mapping.cdm,
        .freq_density             = res_mapping.freq_density,
    };
    csi_rs::convert_freq_domain(pattern_cfg.freq_allocation_ref_idx, res_mapping.fd_alloc, row);

    const csi_rs_pattern_port reserved = get_csi_rs_pattern(pattern_cfg).get_reserved_pattern();

    periodic_occasion occ;
    occ.origin      = {occasion_origin::signal_type::NZP_CSI_RS, static_cast<uint8_t>(res.res_id), 0};
    occ.slot_period = to_underlying(*res.csi_res_period);
    occ.slot_offset = *res.csi_res_offset;
    occ.crbs        = res_mapping.freq_band_rbs;
    for (unsigned sym = 0; sym != NOF_OFDM_SYM_PER_SLOT_NORMAL_CP; ++sym) {
      if (not reserved.symbol_mask.test(sym)) {
        continue;
      }
      for (unsigned re = 0; re != NOF_SUBCARRIERS_PER_RB; ++re) {
        occ.re_masks[sym].set(re, reserved.re_mask.test(re));
      }
    }

    occasions.push_back(occ);
  }

  return occasions;
}

/// \brief Builds the periodic occasions of the cell's periodic CSI-IM resources, as per TS 38.214, Section 5.2.2.4.
///
/// \remark A CSI-IM resource does not represent a transmission by the cell either, but it is still checked against
/// other transmissions: if something lands on a CSI-IM resource's REs, the cell's own signal leaks into what is meant
/// to be an interference-only measurement.
std::vector<periodic_occasion> get_csi_im_occasions(const serving_cell_config& serv_cell_cfg)
{
  std::vector<periodic_occasion> occasions;

  if (not serv_cell_cfg.csi_meas_cfg.has_value()) {
    return occasions;
  }

  for (const csi_im_resource& res : serv_cell_cfg.csi_meas_cfg->csi_im_res_list) {
    if (not res.csi_res_offset.has_value() or not res.csi_res_period.has_value() or
        not res.csi_im_res_element_pattern.has_value()) {
      // Aperiodic CSI-IM resources do not recur, so they cannot be modelled as periodic occasions.
      continue;
    }

    const auto&    pattern         = *res.csi_im_res_element_pattern;
    const unsigned nof_subcarriers = get_csi_im_pattern_nof_subcarriers(pattern.pattern_type);
    const unsigned nof_symbols     = get_csi_im_pattern_nof_symbols(pattern.pattern_type);

    periodic_occasion occ;
    occ.origin      = {occasion_origin::signal_type::CSI_IM, static_cast<uint8_t>(res.res_id), 0};
    occ.slot_period = to_underlying(*res.csi_res_period);
    occ.slot_offset = *res.csi_res_offset;
    occ.crbs        = res.freq_band_rbs;
    for (unsigned sym = pattern.symbol_location, sym_end = sym + nof_symbols; sym != sym_end; ++sym) {
      for (unsigned sc = pattern.subcarrier_location, sc_end = sc + nof_subcarriers; sc != sc_end; ++sc) {
        occ.re_masks[sym].set(sc);
      }
    }

    occasions.push_back(occ);
  }

  return occasions;
}

/// \brief Builds the periodic occasions of the cell's DL-PRS resources, as per TS 38.211, Section 7.4.1.7.
std::vector<periodic_occasion> get_prs_occasions(const prs_config& prs_cfg)
{
  std::vector<periodic_occasion> occasions;

  for (unsigned set_id = 0, nof_sets = prs_cfg.resource_sets.size(); set_id != nof_sets; ++set_id) {
    const prs_resource_set& res_set           = prs_cfg.resource_sets[set_id];
    const crb_interval      crbs              = {res_set.start_prb, res_set.start_prb + res_set.bandwidth_prbs};
    const unsigned          comb_size         = static_cast<unsigned>(res_set.comb_size);
    const unsigned          nof_symbols       = static_cast<unsigned>(res_set.nof_symbols);
    const unsigned          repetition_factor = static_cast<unsigned>(res_set.repetition_factor);
    const unsigned          time_gap          = static_cast<unsigned>(res_set.time_gap);

    for (unsigned res_id = 0, nof_res = res_set.resources.size(); res_id != nof_res; ++res_id) {
      const prs_resource& res = res_set.resources[res_id];

      // The RE mask is the same for every repetition of the resource, as the frequency offset only depends on the
      // symbol index within the resource, not on the slot or repetition index.
      std::array<std::bitset<NOF_SUBCARRIERS_PER_RB>, NOF_OFDM_SYM_PER_SLOT_NORMAL_CP> re_masks{};
      for (unsigned l = 0; l != nof_symbols; ++l) {
        const unsigned k = (res.re_offset + get_prs_freq_offset(res_set.comb_size, l)) % comb_size;
        for (unsigned re = k; re < NOF_SUBCARRIERS_PER_RB; re += comb_size) {
          re_masks[res.symbol_offset + l].set(re);
        }
      }

      for (unsigned rep = 0; rep != repetition_factor; ++rep) {
        periodic_occasion occ;
        occ.origin = {occasion_origin::signal_type::PRS, static_cast<uint8_t>(set_id), static_cast<uint8_t>(res_id)};
        occ.slot_period = res_set.periodicity_slots;
        occ.slot_offset = res_set.slot_offset + res.slot_offset + rep * time_gap;
        occ.crbs        = crbs;
        occ.re_masks    = re_masks;
        occasions.push_back(occ);
      }
    }
  }

  return occasions;
}

/// Appends \c src to \c dst.
void append(std::vector<periodic_occasion>& dst, std::vector<periodic_occasion> src)
{
  dst.insert(dst.end(), std::make_move_iterator(src.begin()), std::make_move_iterator(src.end()));
}

} // namespace

error_type<std::string> ocudu::check_static_resource_collisions(const ran_cell_config& ran)
{
  const serving_cell_config serv_cell_cfg = config_helpers::make_default_ue_cell_config(ran).serv_cell_cfg;

  std::vector<periodic_occasion> occasions = get_ssb_occasions(ran);
  append(occasions, get_nzp_csi_rs_occasions(serv_cell_cfg));
  append(occasions, get_csi_im_occasions(serv_cell_cfg));
  append(occasions, get_prs_occasions(ran.prs_cfg));

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
