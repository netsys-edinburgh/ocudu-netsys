// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "../ue_context.h"
#include "ocudu/e1ap/cu_up/e1ap_cu_up_bearer_context_update.h"
#include "ocudu/ran/pci.h"
#include "ocudu/support/async/async_task.h"
#include <unordered_map>

namespace ocudu {
namespace ocuup {

class cu_up_bearer_context_modification_routine
{
public:
  cu_up_bearer_context_modification_routine(ue_context&                                     ue_ctxt,
                                            const e1ap_bearer_context_modification_request& msg,
                                            const std::unordered_map<uint64_t, pci_t>&      du_pci_map_);

  void operator()(coro_context<async_task<e1ap_bearer_context_modification_response>>& ctx);

  static const char* name() { return "CU-UP bearer context modification routine"; }

private:
  ue_context&                                        ue_ctxt;
  const e1ap_bearer_context_modification_request&   msg;
  const std::unordered_map<uint64_t, pci_t>&         du_pci_map;
  ocudulog::basic_logger&                            logger;

  // Helper variables
  e1ap_bearer_context_modification_response response = {};
  security::sec_as_config                   security_info;
};

} // namespace ocuup
} // namespace ocudu
