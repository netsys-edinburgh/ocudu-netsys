// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "external/nlohmann/json.hpp"
#include "ocudu/ran/pci.h"
#include "ocudu/support/timers.h"
#include <string>

namespace ocudu {

struct pdcp_tx_metrics_container;
struct pdcp_rx_metrics_container;

namespace app_helpers {
namespace json_generators {

/// Generates a nlohmann JSON object that codifies the given PDCP metrics.
nlohmann::json generate(const pdcp_tx_metrics_container& tx,
                        const pdcp_rx_metrics_container& rx,
                        double                           tx_cpu_usage,
                        double                           rx_cpu_usage,
                        timer_duration                   metrics_period,
                        pci_t                            pci = INVALID_PCI);

/// Generates a string in JSON format that codifies the given PDCP metrics.
std::string generate_string(const pdcp_tx_metrics_container& tx,
                            const pdcp_rx_metrics_container& rx,
                            double                           tx_cpu_usage,
                            double                           rx_cpu_usage,
                            timer_duration                   metrics_period,
                            int                              indent = -1,
                            pci_t                            pci    = INVALID_PCI);

} // namespace json_generators
} // namespace app_helpers
} // namespace ocudu
