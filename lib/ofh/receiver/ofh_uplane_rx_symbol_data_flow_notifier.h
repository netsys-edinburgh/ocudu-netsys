// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-FileCopyrightText: Copyright (C) 2026 OCUDU contributors
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "../support/uplink_context_repository.h"
#include "ocudu/ocudulog/logger.h"
#include "ocudu/ofh/ofh_uplane_rx_symbol_notifier.h"

namespace ocudu {
namespace ofh {

/// Open Fronthaul User-Plane uplink received symbol data flow notifier.
class uplane_rx_symbol_data_flow_notifier
{
public:
  uplane_rx_symbol_data_flow_notifier(ocudulog::basic_logger&                    logger_,
                                      std::shared_ptr<uplink_context_repository> ul_context_repo_,
                                      std::shared_ptr<uplane_rx_symbol_notifier> notifier_) :
    logger(logger_), ul_context_repo(std::move(ul_context_repo_)), notifier(std::move(notifier_))
  {
    ocudu_assert(ul_context_repo, "Invalid uplink context repository");
    ocudu_assert(notifier, "Invalid notifier pointer");
  }

  /// Notifies the received symbol for the given slot and symbol when all the PRBs for all the ports of the grid
  /// have been written.
  void notify_received_symbol(slot_point slot, unsigned symbol)
  {
    notify_received_symbol(slot, symbol, []() {});
  }

  /// \brief Notifies the received symbol for the given slot and symbol when all the PRBs for all the ports of the grid
  /// have been written.
  ///
  /// \param[in] slot          Slot of the received symbol.
  /// \param[in] symbol        Symbol index.
  /// \param[in] before_notify Called when the symbol is complete, after it left the repository and before it is
  ///                          notified (e.g., to submit queued device writes into its grid).
  template <typename BeforeNotify>
  void notify_received_symbol(slot_point slot, unsigned symbol, BeforeNotify&& before_notify)
  {
    expected<uplink_context::uplink_context_resource_grid_info> context =
        ul_context_repo->try_popping_complete_resource_grid_symbol(slot, symbol);
    if (!context) {
      return;
    }

    before_notify();
    notify(*context, symbol);
  }

private:
  /// Notifies the given complete grid symbol.
  void notify(uplink_context::uplink_context_resource_grid_info& ctx_value, unsigned symbol);

  ocudulog::basic_logger&                    logger;
  std::shared_ptr<uplink_context_repository> ul_context_repo;
  std::shared_ptr<uplane_rx_symbol_notifier> notifier;
};

} // namespace ofh
} // namespace ocudu
