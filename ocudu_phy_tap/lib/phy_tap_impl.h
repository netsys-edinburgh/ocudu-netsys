/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#pragma once

#include "external_ul_processor.h"
#include "srsran/phy/support/resource_grid_reader.h"
#include "srsran/phy/upper/phy_tap/phy_tap.h"
#include "srsran/srslog/logger.h"
#include "srsran/srslog/srslog.h"

namespace srsran {

class phy_tap_impl : public phy_tap
{
public:
  // Forbid default constructor.
  phy_tap_impl() = delete;

  explicit phy_tap_impl(std::unique_ptr<external_ul_processor> processor_) :
    processor(std::move(processor_)), logger(srslog::fetch_basic_logger("PHY_TAP", true))
  {
    // TODO: Choose how to set the log level or if logging is required at all in the decorator.
    logger.set_level(srslog::basic_levels::debug);
  }

  /// \brief Handler that exposes the received uplink symbols to any external processing unit.
  void handle_ul_symbol(resource_grid_writer&                                     grid_writer,
                        const resource_grid_reader&                               grid_reader,
                        slot_point                                                slot,
                        unsigned                                                  symbol,
                        span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
                        span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
                        span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
                        span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) override
  {
    logger.debug("Received symbol: slot={}, symbol={}", slot, symbol);

    // Log each received PDU.
    for (const auto& pusch_pdu : pusch_pdus) {
      const auto& pdu = pusch_pdu.pdu;
      logger.debug("  PUSCH PDU: rnti={:#x}, symb=[{}, {})",
                   pdu.rnti,
                   pdu.start_symbol_index,
                   pdu.start_symbol_index + pdu.nof_symbols);
    }

    for (const auto& pucch_pdu : pucch_pdus) {
      logger.debug("  PUCCH PDU: rnti={}, format={})", pucch_pdu.context.rnti, to_string(pucch_pdu.context.format));
    }

    for (const auto& pucch_f1_pdu : pucch_f1_pdus) {
      logger.debug("  PUCCH Format 1 PDU: symb=[{}, {})",
                   pucch_f1_pdu.start_symbol_index,
                   pucch_f1_pdu.start_symbol_index + pucch_f1_pdu.nof_symbols);
    }

    for (const auto& srs_pdu : srs_pdus) {
      logger.debug("  SRS PDU: rnti {}, symb=[{}, {})",
                   srs_pdu.context.rnti,
                   srs_pdu.config.resource.start_symbol,
                   srs_pdu.config.resource.start_symbol.value() +
                       static_cast<unsigned>(srs_pdu.config.resource.nof_symbols));
    }

    // Apply the external processing.
    processor->process(grid_writer, grid_reader, slot, symbol);
  }

private:
  /// UL symbol processor for processing the received symbols.
  std::unique_ptr<external_ul_processor> processor;
  /// Logger object.
  srslog::basic_logger& logger;
};

} // namespace srsran
