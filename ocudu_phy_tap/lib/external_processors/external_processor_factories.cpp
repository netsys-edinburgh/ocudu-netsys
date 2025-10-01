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
#include "external_ul_processor_example_impl.h"

using namespace srsran;

namespace {

/// Factory that creates an example external uplink processor.
class external_ul_processor_example_factory : public external_ul_processor_factory
{
public:
  /// Factory constructor.
  external_ul_processor_example_factory(unsigned           nof_rb_,
                                        unsigned           nof_ports_,
                                        const std::string& processor_arguments_) :
    nof_rb(nof_rb_), nof_ports(nof_ports_), processor_arguments(processor_arguments_)
  {
  }

  // See interface for documentation.
  std::unique_ptr<external_ul_processor> create() override
  {
    // Create and return the dummy external UL processor.
    return std::make_unique<external_ul_processor_example_impl>(nof_rb, nof_ports, processor_arguments);
  }

private:
  /// Number of resource blocks to process in the resource grid.
  unsigned nof_rb;
  /// Number of ports to process.
  unsigned nof_ports;
  /// External processor arguments.
  std::string processor_arguments;
};

// [EXTERNAL CODE INSERTION START] Define your own external UL processor factories here.

// [EXTERNAL CODE INSERTION END]

} // namespace

// See interface for documentation.
std::shared_ptr<external_ul_processor_factory>
srsran::create_external_ul_procesor_example_factory(unsigned           nof_rb,
                                                    unsigned           nof_ports,
                                                    const std::string& processor_arguments)
{
  // Create and return the external UL processor dummy factory.
  return std::make_shared<external_ul_processor_example_factory>(nof_rb, nof_ports, processor_arguments);
}

// [EXTERNAL CODE INSERTION START] Define your own external UL processor factory creation functions here.

// [EXTERNAL CODE INSERTION END]
