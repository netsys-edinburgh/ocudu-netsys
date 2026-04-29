// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "external_processor_factories.h"
#include "phy_tap_impl.h"
#include "ocudu/phy/upper/upper_phy_factories.h"

using namespace ocudu;

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
ocudu::create_phy_tap_factory(unsigned nof_rb, unsigned nof_ports, const std::string& processor_arguments)
{
  return std::make_shared<phy_tap_factory_impl>(nof_rb, nof_ports, processor_arguments);
}
