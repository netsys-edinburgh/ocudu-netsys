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
#include <external_processor_factories.h>

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
  explicit upper_phy_rx_symbol_handler_tap_factory_impl(std::shared_ptr<upper_phy_rx_symbol_handler_factory> factory_,
                                                        unsigned                                             nof_rb_,
                                                        unsigned                                             nof_ports_,
                                                        const std::string& processor_arguments_) :
    base_factory(std::move(factory_))
  {
    srsran_assert(base_factory, "Invalid Rx symbol handler factory.");

    // [EXTERNAL CODE INSERTION START] Use your own external UL processor factory creation function here.

    // Create the external UL processor factory.
    processor_factory = create_external_ul_procesor_dummy_factory(nof_rb_, nof_ports_, processor_arguments_);

    // [EXTERNAL CODE INSERTION END]

    report_fatal_error_if_not(processor_factory, "Invalid external UL processor factory.");
  }

  /// Creates a new upper physical layer receive symbol handler tap decorator.
  std::unique_ptr<upper_phy_rx_symbol_handler> create(uplink_slot_processor_pool& ul_processor_pool_) override
  {
    // Create the RX symbol handler.
    std::unique_ptr<upper_phy_rx_symbol_handler> rx_symbol_handler = base_factory->create(ul_processor_pool_);

    // Create the external UL processor.
    std::unique_ptr<external_ul_processor> external_processor = processor_factory->create();

    // Create and return the RX symbol handler tap decorator.
    return std::make_unique<upper_phy_rx_symbol_handler_tap_decorator>(std::move(rx_symbol_handler),
                                                                       std::move(external_processor));
  }

private:
  /// Factory that creates RX symbol handlers.
  std::shared_ptr<upper_phy_rx_symbol_handler_factory> base_factory;
  /// Factory used to create external UL processors.
  std::shared_ptr<external_ul_processor_factory> processor_factory;
};

} // namespace

// See interface for documentation.
std::shared_ptr<upper_phy_rx_symbol_handler_factory>
srsran::create_rx_symbol_handler_tap_factory(std::shared_ptr<upper_phy_rx_symbol_handler_factory> factory,
                                             unsigned                                             nof_rb,
                                             unsigned                                             nof_ports,
                                             const std::string&                                   processor_arguments)
{
  // Create and return the RX symbol handler tap factory.
  return std::make_shared<upper_phy_rx_symbol_handler_tap_factory_impl>(
      std::move(factory), nof_rb, nof_ports, processor_arguments);
}
