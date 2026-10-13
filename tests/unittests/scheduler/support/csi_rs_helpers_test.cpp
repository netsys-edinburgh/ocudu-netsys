// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/scheduler/support/csi_rs_helpers.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

constexpr unsigned      nof_prbs = 106;
const crb_interval      all_crbs{0, nof_prbs};
const ofdm_symbol_range pdsch_symbols{1, 14};

/// Single-port CSI-RS in one OFDM symbol, as for a TRS (TS 38.211, Table 7.4.1.5.3-1, row 1, three RE per PRB) or a
/// row 2 CSI-RS (one RE per PRB, or per every other PRB).
csi_rs_info make_single_port_csi_rs(unsigned                 row,
                                    csi_rs_freq_density_type density,
                                    unsigned                 symbol,
                                    unsigned                 fd_bit,
                                    crb_interval             crbs = all_crbs)
{
  csi_rs_info csi_rs{};
  csi_rs.crbs        = crbs;
  csi_rs.type        = row == 1 ? csi_rs_type::TRS : csi_rs_type::CSI_RS_NZP;
  csi_rs.row         = row;
  csi_rs.freq_domain = csi_rs::freq_allocation_mask_type(row == 1 ? 4 : 12);
  csi_rs.freq_domain.set(fd_bit);
  csi_rs.symbol0      = symbol;
  csi_rs.symbol1      = 0;
  csi_rs.cdm_type     = csi_rs_cdm_type::no_CDM;
  csi_rs.freq_density = density;
  return csi_rs;
}

/// TRS of one slot: two OFDM symbols with three resource elements per PRB.
std::vector<csi_rs_info> make_trs_slot()
{
  return {make_single_port_csi_rs(1, csi_rs_freq_density_type::three, 4, 0),
          make_single_port_csi_rs(1, csi_rs_freq_density_type::three, 8, 0)};
}

} // namespace

TEST(csi_rs_helpers_test, no_csi_rs_reserves_no_resource_elements)
{
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re({}, pdsch_symbols, all_crbs), 0);
}

TEST(csi_rs_helpers_test, trs_reserves_three_resource_elements_per_prb_and_symbol)
{
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(make_trs_slot(), pdsch_symbols, all_crbs), nof_prbs * 2 * 3);
}

TEST(csi_rs_helpers_test, only_the_pdsch_symbols_and_crbs_are_counted)
{
  // The PDSCH starts after the first TRS symbol.
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(make_trs_slot(), {5, 14}, all_crbs), nof_prbs * 3);
  // The PDSCH covers part of the CRBs of a CSI-RS that covers part of the band.
  std::vector<csi_rs_info> csi_rs_list = {
      make_single_port_csi_rs(1, csi_rs_freq_density_type::three, 4, 0, crb_interval{0, 52})};
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(csi_rs_list, pdsch_symbols, crb_interval{40, nof_prbs}), 12 * 3);
}

TEST(csi_rs_helpers_test, resource_elements_shared_by_several_csi_rs_are_counted_once)
{
  // The same TRS twice, e.g., an NZP-CSI-RS and a ZP-CSI-RS on the same resource elements.
  std::vector<csi_rs_info> csi_rs_list = make_trs_slot();
  csi_rs_list.push_back(csi_rs_list.front());
  csi_rs_list.back().type = csi_rs_type::CSI_RS_ZP;
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(csi_rs_list, pdsch_symbols, all_crbs), nof_prbs * 2 * 3);

  // A CSI-RS on other resource elements of the same symbol adds to the count.
  csi_rs_list.push_back(make_single_port_csi_rs(2, csi_rs_freq_density_type::one, 4, 1));
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(csi_rs_list, pdsch_symbols, all_crbs), nof_prbs * (2 * 3 + 1));
}

TEST(csi_rs_helpers_test, density_one_half_occupies_every_other_prb)
{
  std::vector<csi_rs_info> csi_rs_list = {make_single_port_csi_rs(2, csi_rs_freq_density_type::dot5_even_RB, 4, 0)};
  ASSERT_EQ(csi_helper::get_nof_csi_rs_re(csi_rs_list, pdsch_symbols, all_crbs), nof_prbs / 2);
}
