/*
 *
 * Copyright 2021-2025 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "external_processor_factories.h"
#include "phy_tap_impl.h"
#include "srsran/phy/upper/upper_phy_factories.h"

using namespace srsran;

namespace {

/// Factory interface for creating upper physical layer tap. This factory msut be defined in the PHY tap plugin.
class phy_tap_factory_impl : public phy_tap_factory
{
public:
  explicit phy_tap_factory_impl(unsigned nof_rb_, unsigned nof_ports_, const std::string& processor_arguments_)
  {
    // [EXTERNAL CODE INSERTION START] Use your own external UL processor factory creation function here.

    // Create the external UL processor factory.
    processor_factory = create_external_ul_procesor_example_factory(nof_rb_, nof_ports_, processor_arguments_);

    // [EXTERNAL CODE INSERTION END]

    report_fatal_error_if_not(processor_factory, "Invalid external UL processor factory.");
  }

  /// \brief Creates a new upper physical layer tap.
  std::unique_ptr<phy_tap> create() override
  {
    std::unique_ptr<external_ul_processor> processor = processor_factory->create();
    report_fatal_error_if_not(processor, "Invalid external UL processor.");

    return std::make_unique<phy_tap_impl>(std::move(processor));
  }

private:
  /// Factory used to create external UL processors.
  std::shared_ptr<external_ul_processor_factory> processor_factory;
};

} // namespace

std::shared_ptr<phy_tap_factory>
srsran::create_phy_tap_factory(unsigned nof_rb, unsigned nof_ports, const std::string& processor_arguments)
{
  return std::make_shared<phy_tap_factory_impl>(nof_rb, nof_ports, processor_arguments);
}
