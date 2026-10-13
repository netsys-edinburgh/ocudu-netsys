// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/aurora/aurora.h"
#include "ocudu/f1ap/f1ap_message.h"

namespace ocudu {
namespace f1c_aurora {

/// \brief Reports one F1AP message crossing the F1-C gateway to AURORA.
///
/// \param nof_bytes encoded size of the message, or empty when the gateway does not encode it (co-located DU and
/// CU-CP), in which case it is encoded here to measure it.
void record_message(const f1ap_message& msg, aurora::f1c_dir dir, std::optional<uint32_t> nof_bytes);

/// Reports a received F1AP PDU that could not be decoded.
void record_decode_error(aurora::f1c_dir dir);

/// \brief F1 link state of one F1-C association.
///
/// The link counts as up from the F1 Setup Response that goes through the association until the association is
/// destroyed, which happens when the DU disconnects.
class link_tracker
{
public:
  link_tracker() = default;
  link_tracker(const link_tracker&)            = delete;
  link_tracker& operator=(const link_tracker&) = delete;
  ~link_tracker();

  /// To be called with every DL (CU-CP -> DU) message of the association.
  void on_dl_message(const f1ap_message& msg);

private:
  bool up = false;
};

} // namespace f1c_aurora
} // namespace ocudu
