/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "rx_symbol_handler_tap_decorator.h"
#include "srsran/phy/upper/upper_phy_factories.h"
#include "srsran/srslog/logger.h"

using namespace srsran;

namespace {

/// Factory that creates RX symbol handler tap decorators.
class upper_phy_rx_symbol_handler_tap_factory_impl : public upper_phy_rx_symbol_handler_factory
{
public:
  /// \brief Factory constructor.
  ///
  /// It takes an RX symbol handler factory that is used to create RX symbol handler instances. Each instance is
  /// injected into the tap decorator upon creation.
  explicit upper_phy_rx_symbol_handler_tap_factory_impl(std::unique_ptr<upper_phy_rx_symbol_handler_factory> factory_,
                                                        srslog::basic_logger&                                logger_) :
    rx_symbol_handler_factory(std::move(factory_)), logger(logger_)
  {
    srsran_assert(rx_symbol_handler_factory, "Invalid Rx symbol handler factory.");
  }

  /// Creates a new upper PHY RX symbol handler tap decorator.
  std::unique_ptr<upper_phy_rx_symbol_handler> create(uplink_slot_processor_pool& ul_processor_pool_) override
  {
    // Create the RX symbol handler.
    std::unique_ptr<upper_phy_rx_symbol_handler> rx_symbol_handler =
        rx_symbol_handler_factory->create(ul_processor_pool_);

    // Create and return the RX symbol handler tap decorator.
    return std::make_unique<upper_phy_rx_symbol_handler_tap_decorator>(std::move(rx_symbol_handler), logger);
  }

private:
  /// Factory that creates RX symbol handlers.
  std::unique_ptr<upper_phy_rx_symbol_handler_factory> rx_symbol_handler_factory;
  /// Logger object.
  srslog::basic_logger& logger;
};

} // namespace

/// \brief Creates a RX symbol handler factory.
std::unique_ptr<upper_phy_rx_symbol_handler_factory>
srsran::create_rx_symbol_handler_tap_factory(std::unique_ptr<upper_phy_rx_symbol_handler_factory> factory,
                                             srslog::basic_logger&                                logger_)
{
  // Create and return the RX symbol handler tap factory.
  return std::make_unique<upper_phy_rx_symbol_handler_tap_factory_impl>(std::move(factory), logger_);
}
