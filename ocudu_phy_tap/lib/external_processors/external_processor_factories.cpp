// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "external_ul_processor_example_impl.h"
#include "external_ul_processor_factory.h"
#include "srs_iq_dump_zmq.h"
#include "tap_ul_resource_grid_epre_zmq.h"
#include "zmq_server_backend.h"
#include "ocudu/support/executors/task_worker.h"

using namespace ocudu;

namespace {

/// Factory that creates an example external uplink processor.
class external_ul_processor_example_factory : public external_ul_processor_factory
{
public:
  /// Factory constructor.
  external_ul_processor_example_factory(unsigned                               nof_rb_,
                                        unsigned                               nof_ports_,
                                        std::optional<tdd_ul_dl_config_common> tdd_pattern_,
                                        const std::string&                     processor_arguments_) :
    nof_rb(nof_rb_), nof_ports(nof_ports_), tdd_pattern(tdd_pattern_), processor_arguments(processor_arguments_)
  {
  }

  // See interface for documentation.
  std::unique_ptr<external_ul_processor> create() override
  {
    // Create and return the dummy external UL processor.
    return std::make_unique<external_ul_processor_example_impl>(nof_rb, nof_ports, tdd_pattern, processor_arguments);
  }

private:
  /// Number of resource blocks to process in the resource grid.
  unsigned nof_rb;
  /// Number of ports to process.
  unsigned nof_ports;
  /// TDD configuration.
  std::optional<tdd_ul_dl_config_common> tdd_pattern;
  /// External processor arguments.
  std::string processor_arguments;
};

/// \brief Factory for creating uplink resource grid energy per subcarrier tap processors.
///
/// This factory class implements the \ref external_ul_processor_factory interface to create tap processors that monitor
/// and analyze uplink resource grid data. The created processors calculate energy per subcarrier measurements and
/// transmit them to a ZeroMQ backend.
class tap_ul_resource_grid_epre_zmq_factory : public external_ul_processor_factory
{
public:
  /// \brief Constructs the tap processor factory with base factory and ZeroMQ backend address.
  ///
  /// \param base_factory_        Optional base processor factory for chaining operations.
  /// \param backend_zmq_address_ The ZeroMQ address to bind.
  tap_ul_resource_grid_epre_zmq_factory(std::shared_ptr<external_ul_processor_factory> base_factory_,
                                        const std::string&                             backend_zmq_address_) :
    base_factory(std::move(base_factory_)), backend_zmq_address(backend_zmq_address_)
  {
    ocudu_assert(!backend_zmq_address.empty(), "The ZMQ address must cannot be empty.");
  }

  // See the external_ul_processor_factory interface for documentation.
  std::unique_ptr<external_ul_processor> create() override
  {
    // Create shared dependencies if they have not been created yet.
    if (!shared_deps) {
      shared_deps = std::make_shared<tap_ul_resource_grid_epre_zmq::dependencies>(backend_zmq_address);
    }

    // Create base instance if the base factory is present.
    std::unique_ptr<external_ul_processor> base_instance;
    if (base_factory) {
      base_instance = base_factory->create();
    }

    // Create actual processor wrapper.
    return std::make_unique<tap_ul_resource_grid_epre_zmq>(std::move(base_instance), shared_deps);
  }

private:
  /// Shared dependencies for all the taps.
  std::shared_ptr<tap_ul_resource_grid_epre_zmq::dependencies> shared_deps;
  /// Base external UL processor factory.
  std::shared_ptr<external_ul_processor_factory> base_factory;
  /// ZMQ backend address to bind.
  std::string backend_zmq_address;
};

/// \brief Factory for creating SRS occasion IQ dump tap processors.
///
/// This factory class implements the \ref external_ul_processor_factory interface to create tap processors that
/// stream the raw IQ samples of every completed SRS occasion to a ZeroMQ backend, for out-of-process capture.
class srs_iq_dump_zmq_factory : public external_ul_processor_factory
{
public:
  /// \brief Constructs the tap processor factory with base factory and ZeroMQ backend address.
  ///
  /// \param base_factory_        Optional base processor factory for chaining operations.
  /// \param backend_zmq_address_ The ZeroMQ address to bind.
  /// \param nof_rb_              Number of resource blocks in the resource grid, used to size the buffer pool.
  srs_iq_dump_zmq_factory(std::shared_ptr<external_ul_processor_factory> base_factory_,
                          const std::string&                             backend_zmq_address_,
                          unsigned                                       nof_rb_) :
    base_factory(std::move(base_factory_)), backend_zmq_address(backend_zmq_address_), nof_rb(nof_rb_)
  {
    ocudu_assert(!backend_zmq_address.empty(), "The ZMQ address must cannot be empty.");
  }

  // See the external_ul_processor_factory interface for documentation.
  std::unique_ptr<external_ul_processor> create() override
  {
    // Create shared dependencies if they have not been created yet.
    if (!shared_deps) {
      shared_deps = std::make_shared<srs_iq_dump_zmq::dependencies>(backend_zmq_address, nof_rb);
    }

    // Create base instance if the base factory is present.
    std::unique_ptr<external_ul_processor> base_instance;
    if (base_factory) {
      base_instance = base_factory->create();
    }

    // Create actual processor wrapper.
    return std::make_unique<srs_iq_dump_zmq>(std::move(base_instance), shared_deps);
  }

private:
  /// Shared dependencies for all the taps.
  std::shared_ptr<srs_iq_dump_zmq::dependencies> shared_deps;
  /// Base external UL processor factory.
  std::shared_ptr<external_ul_processor_factory> base_factory;
  /// ZMQ backend address to bind.
  std::string backend_zmq_address;
  /// Number of resource blocks in the resource grid.
  unsigned nof_rb;
};

// [EXTERNAL CODE INSERTION START] Define your own external UL processor factories here.

// [EXTERNAL CODE INSERTION END]

} // namespace

// See interface for documentation.
std::shared_ptr<external_ul_processor_factory>
ocudu::create_external_ul_procesor_example_factory(unsigned                               nof_rb,
                                                   unsigned                               nof_ports,
                                                   std::optional<tdd_ul_dl_config_common> tdd_pattern,
                                                   const std::string&                     processor_arguments)
{
  // Create base plugin factory.
  std::shared_ptr<external_ul_processor_factory> factory =
      std::make_shared<external_ul_processor_example_factory>(nof_rb, nof_ports, tdd_pattern, processor_arguments);

  // Try parsing the UL tap ZMQ binding address.
  std::smatch match;
  std::regex  backend_zmq_address_regex(R"(tap_ul_epre=([^,]*))");
  if (std::regex_search(processor_arguments, match, backend_zmq_address_regex)) {
    factory = std::make_shared<tap_ul_resource_grid_epre_zmq_factory>(std::move(factory), match[1].str());
  }

  // Try parsing the SRS IQ dump ZMQ binding address.
  std::regex srs_iq_dump_zmq_address_regex(R"(srs_iq_dump=([^,]*))");
  if (std::regex_search(processor_arguments, match, srs_iq_dump_zmq_address_regex)) {
    factory = std::make_shared<srs_iq_dump_zmq_factory>(std::move(factory), match[1].str(), nof_rb);
  }

  // Create and return the external UL processor dummy factory.
  return factory;
}

// [EXTERNAL CODE INSERTION START] Define your own external UL processor factory creation functions here.

// [EXTERNAL CODE INSERTION END]
