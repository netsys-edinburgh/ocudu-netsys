// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "external_ul_processor.h"
#include "zmq_server_backend.h"
#include "ocudu/adt/detail/concurrent_queue_params.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/cyclic_prefix.h"
#include "ocudu/ran/prach/prach_constants.h"
#include "ocudu/ran/srs/srs_constants.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/executors/task_worker.h"
#include "ocudu/support/memory_pool/bounded_object_pool.h"

namespace ocudu {

/// \brief Wire header describing an SRS IQ dump payload, sent as the first part of a two-part ZMQ message.
///
/// The payload (second message part) holds \c nof_symbols * \c nof_ports * \c nof_subc consecutive \c cf_t samples,
/// ordered as: symbol (outer), port, subcarrier (inner). This layout matches \c numpy.complex64 directly, so a
/// receiver can reshape the payload without any parsing beyond this header.
///
/// \note The struct is packed to remove compiler-inserted padding, so its wire size exactly matches the sum of its
/// field sizes (31 bytes) - this must be kept in sync with any receiver-side unpacking format.
#pragma pack(push, 1)
struct srs_iq_dump_header {
  /// Magic number identifying this message type ("SRSI" in ASCII).
  uint32_t magic = 0x53525349;
  /// Wire format version, bumped on any incompatible layout change.
  uint16_t version = 1;
  /// RNTI of the UE that transmitted this SRS occasion.
  uint16_t rnti;
  /// System Frame Number of the slot the occasion completed in.
  uint32_t sfn;
  /// Slot index within the frame.
  uint16_t slot_index;
  /// Subcarrier spacing, in kHz.
  uint16_t scs_khz;
  /// First OFDM symbol of the occasion within the slot.
  uint8_t start_symbol;
  /// Number of consecutive OFDM symbols in the occasion.
  uint8_t nof_symbols;
  /// Number of receive ports captured.
  uint8_t nof_ports;
  /// Number of subcarriers captured per symbol/port.
  uint16_t nof_subc;
  /// SRS comb size (2 or 4).
  uint8_t comb_size;
  /// SRS comb offset.
  uint8_t comb_offset;
  /// SRS cyclic shift.
  uint8_t cyclic_shift;
  /// SRS sequence identifier.
  uint16_t sequence_id;
  /// SRS bandwidth configuration index, parameter C_SRS.
  uint8_t configuration_index;
  /// SRS bandwidth index, parameter B_SRS.
  uint8_t bandwidth_index;
  /// SRS frequency domain position, parameter n_RRC.
  uint8_t freq_position;
  /// SRS frequency domain shift, parameter n_shift.
  uint16_t freq_shift;
};
#pragma pack(pop)

static_assert(sizeof(srs_iq_dump_header) == 31, "Unexpected srs_iq_dump_header size - update the receiver format.");

/// \brief ZeroMQ backend for dumping raw SRS occasion IQ samples.
///
/// This class implements an external uplink processor that, for every SRS occasion completed by the PHY, copies the
/// occasion's IQ samples out of the resource grid and streams them - together with a header describing the occasion
/// - to a ZeroMQ backend for out-of-process capture and analysis.
class srs_iq_dump_zmq : public external_ul_processor
{
public:
  /// Maximum number of OFDM symbols an SRS occasion can span, as per TS 38.211.
  static constexpr unsigned max_srs_symbols = 4;
  /// Maximum number of receive ports supported.
  static constexpr unsigned max_srs_ports = 4;

  /// \brief Collects the necessary dependencies for the SRS IQ dump processor.
  ///
  /// Mirrors \ref tap_ul_resource_grid_epre_zmq::dependencies: a dedicated background thread and executor keep the
  /// ZeroMQ send call off the real-time RU execution context, and a bounded pool of pre-sized buffers avoids heap
  /// allocation on that same real-time path.
  class dependencies
  {
  public:
    /// \brief Constructs the dependencies with the specified backend address.
    ///
    /// \param backend_address Th e ZeroMQ address to bind.
    /// \param nof_rb           Number of resource blocks in the resource grid, used to size the buffer pool.
    dependencies(std::string backend_address, unsigned nof_rb) :
      worker("SRSIQDump", default_queue_size),
      executor(worker),
      backend(backend_address),
      buffers(default_nof_buffers, nof_rb * NOF_SUBCARRIERS_PER_RB * max_srs_symbols * max_srs_ports)
    {
    }

    /// Destructor that stops the internal task worker thread.
    ~dependencies() { worker.stop(); }

    /// Retrieves the task executor for asynchronous operations.
    task_executor& get_executor() { return executor; }

    /// Retrieves the ZeroMQ backend for sending the IQ dumps.
    zmq_server_backend& get_backend() { return backend; }

    /// Retrieves the shared pool of IQ sample buffers.
    bounded_object_pool<std::vector<cf_t>>& get_buffer_pool() { return buffers; }

  private:
    /// Default queue size for the task worker.
    static constexpr unsigned default_queue_size = 2048;
    /// Default number of buffers in the pool - allows several occasions from co-scheduled UEs to be in flight.
    static constexpr unsigned default_nof_buffers = 16;
    /// Default queue policy for concurrent operations.
    static constexpr concurrent_queue_policy default_queue_policy = concurrent_queue_policy::locking_mpsc;

    /// Internal task worker thread.
    general_task_worker<default_queue_policy> worker;
    /// Executor implementation.
    general_task_worker_executor<default_queue_policy> executor;
    /// ZeroMQ backend.
    zmq_server_backend backend;
    /// Pool of pre-allocated IQ sample buffers.
    bounded_object_pool<std::vector<cf_t>> buffers;
  };

  /// \brief Constructor that initializes the SRS IQ dump processor.
  ///
  /// \param base_instance_ Optional base processor instance for chaining operations.
  /// \param deps_          Shared dependencies required for processor operation.
  srs_iq_dump_zmq(std::unique_ptr<external_ul_processor> base_instance_, std::shared_ptr<dependencies> deps_) :
    logger(ocudulog::fetch_basic_logger("PHY_TAP", true)), base_instance(std::move(base_instance_)), deps(std::move(deps_))
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

private:
  /// Captures and sends the IQ samples for a single completed SRS occasion.
  void dump_occasion(const resource_grid_reader& grid_reader, const uplink_pdu_slot_repository::srs_pdu& srs_pdu);

  /// Logger object.
  ocudulog::basic_logger& logger;
  /// Optional instance for chaining.
  std::unique_ptr<external_ul_processor> base_instance;
  /// Shared dependencies.
  std::shared_ptr<dependencies> deps;
};

} // namespace ocudu
