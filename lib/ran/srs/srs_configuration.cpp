// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/ran/srs/srs_configuration.h"
#include "ocudu/ran/srs/srs_resource_configuration.h"

using namespace ocudu;

srs_resource_configuration ocudu::to_srs_resource_configuration(const srs_config::srs_resource& res)
{
  srs_resource_configuration res_cfg{};
  res_cfg.nof_antenna_ports   = static_cast<srs_resource_configuration::one_two_four_enum>(res.nof_ports);
  res_cfg.configuration_index = res.freq_hop.c_srs;
  res_cfg.sequence_id         = res.sequence_id;
  res_cfg.bandwidth_index     = res.freq_hop.b_srs;
  res_cfg.comb_size           = res.tx_comb.size;
  res_cfg.comb_offset         = res.tx_comb.tx_comb_offset;
  res_cfg.cyclic_shift        = res.tx_comb.tx_comb_cyclic_shift;
  res_cfg.freq_position       = res.freq_domain_pos;
  res_cfg.freq_shift          = res.freq_domain_shift;
  res_cfg.freq_hopping        = res.freq_hop.b_hop;
  res_cfg.hopping             = res.grp_or_seq_hop;
  return res_cfg;
}
