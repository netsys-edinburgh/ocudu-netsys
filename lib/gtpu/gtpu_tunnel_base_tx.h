// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/aurora/aurora.h"
#include "gtpu_tunnel_logger.h"
#include "ocudu/gtpu/gtpu_tunnel_common_tx.h"
#include "ocudu/pcap/dlt_pcap.h"
#include <arpa/inet.h>
#include <cstdint>
#include <netinet/in.h>

namespace ocudu {

/// Class used for transmitting GTP-U bearers.
class gtpu_tunnel_base_tx
{
public:
  /// Binds this tunnel to an AURORA per-UE tunnel slot (Group 15).
  void set_aurora_slot(unsigned ue_index, int slot)
  {
    aurora_ue   = ue_index;
    aurora_slot = slot;
  }

  gtpu_tunnel_base_tx(gtpu_tunnel_log_prefix                      log_prefix,
                      dlt_pcap&                                   gtpu_pcap_,
                      gtpu_tunnel_common_tx_upper_layer_notifier& upper_dn_) :
    logger("GTPU", log_prefix), gtpu_pcap(gtpu_pcap_), upper_dn(upper_dn_)
  {
  }

  bool to_sockaddr(sockaddr_storage& out_sockaddr, const char* addr, uint16_t port)
  {
    // Validate configuration
    if (inet_pton(AF_INET, addr, &((::sockaddr_in*)&out_sockaddr)->sin_addr) == 1) {
      ((::sockaddr_in*)&out_sockaddr)->sin_family = AF_INET;
      ((::sockaddr_in*)&out_sockaddr)->sin_port   = htons(port);
    } else if (inet_pton(AF_INET6, addr, &((::sockaddr_in6*)&out_sockaddr)->sin6_addr) == 1) {
      ((::sockaddr_in6*)&out_sockaddr)->sin6_family = AF_INET6;
      ((::sockaddr_in6*)&out_sockaddr)->sin6_port   = htons(port);
    } else {
      logger.log_error("Invalid address or port. addr={} port={} errno={}", addr, port, ::strerror(errno));
      return false;
    }
    return true;
  }

protected:
  gtpu_tunnel_logger logger;

  void aurora_add(aurora::gtp_counter c, uint64_t value) const
  {
    if (aurora_slot >= 0) {
      aurora::gtp_add(aurora_ue, static_cast<unsigned>(aurora_slot), c, value);
    }
  }

  unsigned aurora_ue   = 0;
  int      aurora_slot = -1;

  void send_pdu(byte_buffer buf, const ::sockaddr_storage& peer_sockaddr)
  {
    if (gtpu_pcap.is_write_enabled()) {
      auto buf_copy = buf.deep_copy();
      if (not buf_copy.has_value()) {
        logger.log_warning("Unable to deep copy buffer for PCAP writer");
      } else {
        gtpu_pcap.push_pdu(std::move(buf_copy.value()));
      }
    }

    upper_dn.on_new_pdu(std::move(buf), peer_sockaddr);
  }

private:
  dlt_pcap&                                   gtpu_pcap;
  gtpu_tunnel_common_tx_upper_layer_notifier& upper_dn;
};
} // namespace ocudu
