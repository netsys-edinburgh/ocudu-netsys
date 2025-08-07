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

#include "external_ul_symbol_processor.h"
#include "srsran/phy/support/prach_buffer_context.h"
#include "srsran/phy/upper/upper_phy_rx_symbol_handler.h"
#include "srsran/srslog/logger.h"
#include "srsran/srslog/srslog.h"

namespace srsran {

/// Decorator for the upper physical layer receive symbol handler the allows tapping into the received symbols.
class upper_phy_rx_symbol_handler_tap_decorator : public upper_phy_rx_symbol_handler
{
public:
  // Forbid default constructor.
  upper_phy_rx_symbol_handler_tap_decorator() = delete;

  /// Constructor that takes ownership of an RX symbol handler.
  explicit upper_phy_rx_symbol_handler_tap_decorator(std::unique_ptr<upper_phy_rx_symbol_handler> handler_,
                                                     unsigned                                     nof_rb,
                                                     unsigned                                     nof_ports,
                                                     const std::string&                           processor_arguments) :
    handler(std::move(handler_)),
    processor(nof_rb, nof_ports, processor_arguments),
    logger(srslog::fetch_basic_logger("PHY_TAP", true))
  {
    srsran_assert(handler, "Invalid Rx symbol handler.");

    // TODO: Set the log level based on the configuration arguments.
    logger.set_level(srslog::basic_levels::debug);
  }

  // See the interface for documentation.
  void
  handle_rx_symbol(const upper_phy_rx_symbol_context& context, const shared_resource_grid& grid, bool is_valid) override
  {
    // Apply the external processing.
    processor.process(context, grid);

    // Pass the modified grid to the RX symbol handler.
    handler->handle_rx_symbol(context, grid, is_valid);

    logger.debug("Received symbol: sector {}, slot {}, symbol {}, is_valid: {}",
                 context.sector,
                 context.slot,
                 context.symbol,
                 is_valid);
  }

  // See the interface for documentation.
  void handle_rx_prach_window(const prach_buffer_context& context, const prach_buffer& buffer) override
  {
    handler->handle_rx_prach_window(context, buffer);

    logger.debug("Received PRACH: sector {}, slot {}", context.sector, context.slot);
  }

private:
  /// An instance of the upper physical layer receive symbol handler.
  std::unique_ptr<upper_phy_rx_symbol_handler> handler;
  /// UL symbol processor for processing the received symbols.
  external_ul_symbol_processor processor;
  /// Logger object.
  srslog::basic_logger& logger;
};

} // namespace srsran
