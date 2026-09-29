// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../../resource_request_pool.h"
#include "puxch_baseband_demodulator.h"
#include "puxch_processor_demodulator_notifier.h"
#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_ptr_view.h"
#include "ocudu/phy/lower/modulation/ofdm_demodulator.h"
#include "ocudu/phy/lower/processors/lower_phy_center_freq_controller.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_baseband.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_notifier.h"
#include "ocudu/phy/lower/processors/uplink/puxch/puxch_processor_request_handler.h"
#include "ocudu/phy/support/shared_resource_grid.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/synchronization/stop_event.h"
#include <algorithm>
#include <vector>

namespace ocudu {

/// Implements PUxCH baseband processor.
class puxch_processor_impl : public puxch_processor,
                             private puxch_processor_baseband,
                             private lower_phy_center_freq_controller,
                             private puxch_processor_request_handler,
                             public puxch_processor_demodulator_notifier
{
public:
  /// \brief Number of slots the resource grid request array spans.
  ///
  /// Spans 64 slots rather than the default 16 for the same reason as the upper PHY processor assignation: a request
  /// for a slot that far ahead of the one being received must not replace the request of that slot.
  static constexpr unsigned nof_request_slots = 64;

  struct configuration {
    cyclic_prefix cp;
    unsigned      nof_rx_ports;
    unsigned      dft_size;
  };

  puxch_processor_impl(std::unique_ptr<ofdm_symbol_demodulator> demodulator_,
                       task_executor&                           executor,
                       const configuration&                     config) :
    nof_symbols_per_slot(get_nsymb_per_slot(config.cp)),
    common_ofdm_demodulator(std::move(demodulator_)),
    demodulators(max_nof_concurrent_slots)
  {
    ocudu_assert(common_ofdm_demodulator, "Invalid demodulator.");

    // Generate baseband demodulators.
    std::generate(demodulators.begin(), demodulators.end(), [this, &config, &executor]() {
      return std::make_unique<puxch_baseband_demodulator>(
          config.cp, config.nof_rx_ports, executor, *common_ofdm_demodulator, *this);
    });
  }

  // See interface for documentation.
  void connect(puxch_processor_notifier& notifier_) override { notifier = &notifier_; }

  // See interface for documentation.
  void stop() override
  {
    // Don't allow further processing requests.
    stop_control.stop();

    // Release the stored requests.
    requests.clear();

    // Close the current slot, if any.
    current_demodulator.release();

    // Wait for all demodulators to complete their pending tasks.
    std::for_each(demodulators.begin(), demodulators.end(), [](const auto& demodulator) { demodulator->stop(); });
  }

  // See interface for documentation.
  puxch_processor_request_handler& get_request_handler() override { return *this; }

  // See interface for documentation.
  puxch_processor_baseband& get_baseband() override { return *this; }

  // See interface for documentation.
  lower_phy_center_freq_controller& get_center_freq_control() override;

private:
  /// Retrieves the PUxCH demodulator associated to the given slot.
  puxch_baseband_demodulator& get_demodulator(slot_point slot)
  {
    return *demodulators[slot.system_slot() % demodulators.size()];
  }

  // See interface for documentation.
  bool process_symbol(const baseband_gateway_buffer_ptr_view& samples,
                      const lower_phy_rx_symbol_context&      context) override;

  // See interface for documentation.
  void handle_request(const shared_resource_grid& grid, const resource_grid_context& context) override;

  // See interface for documentation.
  bool set_carrier_center_frequency(double carrier_center_frequency_Hz) override;

  // See puxch_processor_demodulator_notifier interface for documentation.
  void on_demodulation_completion(const shared_resource_grid&        grid,
                                  const lower_phy_rx_symbol_context& context) override;

  /// Stop control.
  rt_stop_event_source stop_control;
  /// Number of OFDM symbols per slot.
  unsigned nof_symbols_per_slot;
  /// Physical channel baseband processor event notifier.
  puxch_processor_notifier* notifier = nullptr;
  /// Common OFDM symbol demodulator.
  std::unique_ptr<ofdm_symbol_demodulator> common_ofdm_demodulator;
  /// Uplink physical channel baseband demodulators.
  std::vector<std::unique_ptr<puxch_baseband_demodulator>> demodulators;
  /// \brief Points to a PUxCH demodulator for processing the current slot receiving samples.
  ///
  /// Set to \c nullptr if the current slot is not associated with a receive request.
  puxch_baseband_demodulator_scoped current_demodulator;
  /// Current slot.
  slot_point current_slot;
  /// Pool of receive requests, indexed by slot.
  resource_request_pool<shared_resource_grid, nof_request_slots> requests;
};

} // namespace ocudu
