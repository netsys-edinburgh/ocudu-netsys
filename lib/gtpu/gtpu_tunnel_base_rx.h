// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/aurora/aurora.h"
#include "gtpu_pdu.h"
#include "gtpu_tunnel_logger.h"
#include "ocudu/adt/byte_buffer.h"
#include "ocudu/gtpu/gtpu_tunnel_common_rx.h"

namespace ocudu {

/// Class used for receiving GTP-U bearers.
class gtpu_tunnel_base_rx : public gtpu_tunnel_common_rx_upper_layer_interface
{
public:
  gtpu_tunnel_base_rx(gtpu_tunnel_log_prefix log_prefix, bool test_mode_ = false) :
    logger("GTPU", std::move(log_prefix)), test_mode(test_mode_)
  {
  }
  ~gtpu_tunnel_base_rx() override = default;

  /*
   * SDU/PDU handlers
   */
  void handle_pdu(byte_buffer buf, const sockaddr_storage& src_addr) final
  {
    gtpu_dissected_pdu dissected_pdu;
    if (test_mode) {
      dissected_pdu.test_mode = true;
      dissected_pdu.buf       = std::move(buf);
    } else {
      bool read_ok = gtpu_dissect_pdu(dissected_pdu, std::move(buf), logger);
      if (!read_ok) {
        aurora_add(aurora::gtp_counter::rx_err_pkts, 1);
        logger.log_error("Dropped PDU, error reading GTP-U header. pdu_len={}", dissected_pdu.buf.length());
        return;
      }
    }
    // continue processing in domain-specific subclass
    handle_pdu(std::move(dissected_pdu), src_addr);
  }

  /// Binds this tunnel to an AURORA per-UE tunnel slot (Group 15).
  void set_aurora_slot(unsigned ue_index, int slot)
  {
    aurora_ue   = ue_index;
    aurora_slot = slot;
  }

protected:
  virtual void handle_pdu(gtpu_dissected_pdu&& pdu, const sockaddr_storage& src_addr) = 0;

  void aurora_add(aurora::gtp_counter c, uint64_t value) const
  {
    if (aurora_slot >= 0) {
      aurora::gtp_add(aurora_ue, static_cast<unsigned>(aurora_slot), c, value);
    }
  }

  unsigned aurora_ue   = 0;
  int      aurora_slot = -1;

  gtpu_tunnel_logger logger;
  bool               test_mode;
};
} // namespace ocudu
