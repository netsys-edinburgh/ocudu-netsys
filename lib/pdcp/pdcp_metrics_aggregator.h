// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "pdcp_bearer_logger.h"
#include "ocudu/pdcp/pdcp_metrics.h"
#include "ocudu/pdcp/pdcp_rx_metrics.h"
#include "ocudu/pdcp/pdcp_tx_metrics.h"
#include "ocudu/ran/pci.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/timers.h"
#include <atomic>

namespace ocudu {
class pdcp_metrics_aggregator
{
public:
  pdcp_metrics_aggregator(uint32_t               ue_index_,
                          rb_id_t                rb_id_,
                          timer_duration         metrics_period_,
                          pdcp_metrics_notifier* pdcp_metrics_notif_,
                          task_executor&         ue_executor_,
                          bool                   report_tx_rx_in_same_report_ = true,
                          pci_t                  pci_                         = INVALID_PCI);

  void push_tx_metrics(const pdcp_tx_metrics_container& m_tx_);

  void push_rx_metrics(const pdcp_rx_metrics_container& m_rx_);

  void set_pci(pci_t new_pci) { pci.store(new_pci, std::memory_order_relaxed); }

  const timer_duration& get_metrics_period() const { return metrics_period; }

private:
  void push_report();

  void push_tx_metrics_impl(const pdcp_tx_metrics_container& m_tx_);
  void push_rx_metrics_impl(const pdcp_rx_metrics_container& m_rx_);

  const bool                   report_tx_rx_in_same_report;
  uint32_t                     ue_index;
  rb_id_t                      rb_id;
  std::atomic<uint16_t>        pci;
  pdcp_rx_metrics_container m_rx;
  pdcp_tx_metrics_container m_tx;
  timer_duration            metrics_period;
  pdcp_metrics_notifier*    pdcp_metrics_notif;
  task_executor&            ue_executor;
  pdcp_bearer_logger        logger;
};
} // namespace ocudu
