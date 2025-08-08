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
#include <memory>

namespace srsran {

/// External UL processor factory interface.
class external_ul_processor_factory
{
public:
  /// Default destructor.
  virtual ~external_ul_processor_factory() = default;

  /// Creates an external UL symbol processor instance.
  virtual std::unique_ptr<external_ul_processor> create() = 0;
};

/// \brief Creates an external UL processor factory that produces dummy external UL processors.
///
/// This factory is used to create an external UL processor that applies DSP processing to the UL symbols.
///
/// \param[in] nof_rb              The number of resource blocks to process in the resource grid.
/// \param[in] nof_ports           The number of ports to process.
/// \param[in] processor_arguments Custom arguments for the external processor, if any.
std::shared_ptr<external_ul_processor_factory>
create_external_ul_procesor_dummy_factory(unsigned           nof_rb,
                                          unsigned           nof_ports,
                                          const std::string& processor_arguments = "");

// [EXTERNAL CODE INSERTION START] Declare your own external UL processor factory creation functions here.

// [EXTERNAL CODE INSERTION END]

} // namespace srsran
