// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "external_ul_processor.h"
#include "zmq_server_backend.h"
#include "ocudu/adt/detail/concurrent_queue_params.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/executors/task_worker.h"
#include "ocudu/support/memory_pool/bounded_object_pool.h"

namespace ocudu {

/// \brief ZeroMQ backend for uplink resource grid energy per subcarrier measurements.
///
/// This class implements an external uplink processor that calculates the energy per subcarrier across ports and
/// symbols from the resource grid data. The calculated measurements are then transmitted to a ZeroMQ backend for
/// monitoring and analysis.
class tap_ul_resource_grid_epre_zmq : public external_ul_processor
{
public:
  /// Number of symbols in a slot. Used for quiet slot processing.
  static constexpr unsigned nof_slot_symbols = MAX_NSYMB_PER_SLOT;

  /// Number of temporary buffers for processing.
  static constexpr unsigned nof_temp_buffers = 16;

  /// \brief Collects the necessary dependencies for the uplink tap processor.
  ///
  /// This nested class manages all the required components for the uplink tap processor, including the ZeroMQ backend,
  /// task worker threads, and executor for asynchronous processing.
  class dependencies
  {
  public:
    /// \brief Constructs the dependencies with the specified backend address.
    ///
    /// \param backend_address The ZeroMQ address to bind.
    dependencies(std::string backend_address) :
      worker("ULPhyTap", default_queue_size), executor(worker), backend(backend_address)
    {
    }

    /// Destructor that stops the internal task worker thread.
    ~dependencies() { worker.stop(); }

    /// Retrieves the task executor for asynchronous operations.
    task_executor& get_executor() { return executor; }

    /// Retrieves the ZeroMQ backend for sending measurements.
    zmq_server_backend& get_backend() { return backend; }

  private:
    /// Default queue size for the task worker.
    static constexpr unsigned default_queue_size = 2048;
    /// Default queue policy for concurrent operations.
    static constexpr concurrent_queue_policy default_queue_policy = concurrent_queue_policy::locking_mpsc;

    /// Internal task worker thread.
    general_task_worker<default_queue_policy> worker;
    /// Executor implementation.
    general_task_worker_executor<default_queue_policy> executor;
    /// Uplink tap backend.
    zmq_server_backend backend;
  };

  /// \brief Constructor that initializes the uplink tap processor.
  ///
  /// \param base_instance_ Optional base processor instance for chaining operations.
  /// \param deps_          Shared dependencies required for processor operation.
  tap_ul_resource_grid_epre_zmq(std::unique_ptr<external_ul_processor> base_instance_,
                                std::shared_ptr<dependencies>          deps_) :
    logger(ocudulog::fetch_basic_logger("PHY_TAP", true)),
    base_instance(std::move(base_instance_)),
    deps(std::move(deps_)),
    temp_buffers(nof_temp_buffers)
  {
  }

  // See the interface for documentation.
  void process(resource_grid_writer&                                     grid_writer,
               const resource_grid_reader&                               grid_reader,
               slot_point                                                slot,
               unsigned                                                  symbol,
               span<const uplink_pdu_slot_repository::pusch_pdu>         pusch_pdus,
               span<const uplink_pdu_slot_repository::pucch_pdu>         pucch_pdus,
               span<const pucch_processor::format1_common_configuration> pucch_f1_pdus,
               span<const uplink_pdu_slot_repository::srs_pdu>           srs_pdus) override;

  // See the interface for documentation.
  void process_quiet(const resource_grid_reader& grid_reader, slot_point slot) override;

  // See the interface for documentation.
  void process_prach(prach_buffer& buffer, const prach_buffer_context& context) override;

  /// Logger object.
  ocudulog::basic_logger& logger;
  /// Optional instance for chaining.
  std::unique_ptr<external_ul_processor> base_instance;
  /// Shared dependencies.
  std::shared_ptr<dependencies> deps;
  /// Temporary pool of buffers.
  bounded_object_pool<static_vector<float, MAX_NOF_PRBS * NOF_SUBCARRIERS_PER_RB>> temp_buffers;
  /// Index of the last processed symbol.
  unsigned last_processed_symbol = 0;
  /// Number of the symbols in the current grid.
  unsigned nof_symbols = 0;
  /// Number of ports in the current grid.
  unsigned nof_ports = 0;
  /// Number of subcarrierrs in the current grid.
  unsigned nof_subc = 0;

private:
  /// Computes the EPRE of a set of symbols.
  void compute_epre(const resource_grid_reader& grid_reader);
};

} // namespace ocudu
