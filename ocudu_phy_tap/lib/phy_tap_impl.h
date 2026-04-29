// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "external_ul_processor.h"
#include "ocudu/phy/upper/phy_tap/phy_tap.h"

namespace ocudu {

class phy_tap_impl : public phy_tap
{
public:
  // Forbid default constructor.
  phy_tap_impl() = delete;

  /// Constructor that creates a physical layer tap with the given external UL
  /// processor.
  explicit phy_tap_impl(std::unique_ptr<external_ul_processor> processor_) : processor(std::move(processor_))
  {
    ocudu_assert(processor, "Invalid external UL processor");
  }

  // See interface for documentation.
  void handle_ul_symbol(resource_grid_writer&                                     grid_writer,
                        const resource_grid_reader&                               grid_reader,
                        slot_point                                                slot,
                        unsigned                                                  symbol,
                        span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                        span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                        span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                        span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) override
  {
    // Apply the external processing.
    processor->process(grid_writer, grid_reader, slot, symbol, pusch_pdus, pucch_pdus, pucch_f1_pdus, srs_pdus);
  }

  // See interface for the documentation.
  void handle_prach_window(prach_buffer& buffer, const prach_buffer_context& context) override
  {
    // Apply the external processing.
    processor->process_prach(buffer, context);
  }

  // See interface for documentation.
  void handle_quiet_grid(const resource_grid_reader& grid_reader, slot_point slot) override
  {
    // Apply the external processing.
    processor->process_quiet(grid_reader, slot);
  }

private:
  /// UL symbol processor for processing the received symbols.
  std::unique_ptr<external_ul_processor> processor;
};

} // namespace ocudu
