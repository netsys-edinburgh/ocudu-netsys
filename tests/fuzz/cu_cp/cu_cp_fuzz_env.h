// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// Peer stubs shared by the CU-CP fuzz harnesses.
///
/// A harness attaches these instead of the SCTP gateways, so that it only has to describe what it
/// injects and where.
///
/// See tests/fuzz/README.md for build instructions and run commands.

#pragma once

#include "ocudu/adt/mutexed_mpmc_queue.h"
#include "ocudu/asn1/ngap/common.h"
#include "ocudu/asn1/ngap/ngap.h"
#include "ocudu/asn1/ngap/ngap_ies.h"
#include "ocudu/asn1/ngap/ngap_pdu_contents.h"
#include "ocudu/ngap/gateways/n2_connection_client.h"
#include "ocudu/ngap/ngap_message.h"
#include "ocudu/support/async/async_no_op_task.h"
#include "ocudu/xnap/gateways/xnc_connection_gateway.h"
#include <memory>
#include <optional>

namespace ocudu::ocucp::fuzz {

// ---------------------------------------------------------------------------
// fuzz_amf - replaces the SCTP N2 gateway
// ---------------------------------------------------------------------------

/// Thread-safe N2 connection client used instead of a real SCTP gateway.
class fuzz_amf : public n2_connection_client
{
  using pdu_queue = concurrent_queue<ngap_message,
                                     concurrent_queue_policy::locking_mpmc,
                                     concurrent_queue_wait_policy::condition_variable>;

public:
  fuzz_amf() : rx_pdus(512), pending_auto_tx(16) {}

  std::unique_ptr<ngap_message_notifier>
  handle_cu_cp_connection_request(std::unique_ptr<ngap_rx_message_notifier> notifier) override
  {
    rx_pdu_notifier = std::move(notifier);
    return std::make_unique<tx_notifier>(*this);
  }

  /// Deliver a decoded NGAP message to the CU-CP (AMF to CU-CP direction).
  void push_tx_pdu(const ngap_message& msg)
  {
    if (rx_pdu_notifier) {
      rx_pdu_notifier->on_new_message(msg);
    }
  }

  /// Pre-queue a response the AMF sends in reply to the next CU-CP message.
  void enqueue_auto_response(const ngap_message& msg) { pending_auto_tx.push_blocking(msg); }

  /// Pop the next PDU sent by the CU-CP to the AMF. Returns false if empty.
  bool try_pop_rx_pdu(ngap_message& pdu) { return rx_pdus.try_pop(pdu); }

private:
  class tx_notifier : public ngap_message_notifier
  {
  public:
    explicit tx_notifier(fuzz_amf& parent_) : parent(parent_) {}
    ~tx_notifier() override { parent.rx_pdu_notifier.reset(); }

    [[nodiscard]] bool on_new_message(const ngap_message& msg) override
    {
      ngap_message auto_resp;
      if (parent.pending_auto_tx.try_pop(auto_resp)) {
        parent.push_tx_pdu(auto_resp);
      }
      parent.rx_pdus.push_blocking(msg);
      return true;
    }

  private:
    fuzz_amf& parent;
  };

  std::unique_ptr<ngap_rx_message_notifier> rx_pdu_notifier;
  pdu_queue                                 rx_pdus;
  pdu_queue                                 pending_auto_tx;
};

/// Build the UE Context Release Command answering a UE Context Release Request, as the AMF does in
/// TS 38.413 section 8.3.2. Returns nothing for any other message.
inline std::optional<ngap_message> make_release_command(const ngap_message& msg)
{
  if (msg.pdu.type().value != asn1::ngap::ngap_pdu_c::types_opts::init_msg or
      msg.pdu.init_msg().value.type().value !=
          asn1::ngap::ngap_elem_procs_o::init_msg_c::types_opts::ue_context_release_request) {
    return std::nullopt;
  }
  const auto& req = msg.pdu.init_msg().value.ue_context_release_request();

  ngap_message cmd = {};
  cmd.pdu.set_init_msg();
  cmd.pdu.init_msg().load_info_obj(ASN1_NGAP_ID_UE_CONTEXT_RELEASE);
  auto& release_cmd                      = cmd.pdu.init_msg().value.ue_context_release_cmd();
  auto& ue_id_pair                       = release_cmd->ue_ngap_ids.set_ue_ngap_id_pair();
  ue_id_pair.amf_ue_ngap_id              = req->amf_ue_ngap_id;
  ue_id_pair.ran_ue_ngap_id              = req->ran_ue_ngap_id;
  release_cmd->cause.set_radio_network() = asn1::ngap::cause_radio_network_opts::options::radio_conn_with_ue_lost;
  return cmd;
}

// ---------------------------------------------------------------------------
// fuzz_xnc_gateway - no-op Xn-C connection gateway
// ---------------------------------------------------------------------------

class fuzz_xnc_gateway : public xnc_connection_gateway
{
public:
  async_task<bool> connect_to_peer(std::vector<transport_layer_address> /*peer_addrs*/) override
  {
    return launch_no_op_task(true);
  }
  void                    attach_cu_cp(cu_cp_xnc_handler& /*handler*/) override {}
  void                    stop() override {}
  std::optional<uint16_t> get_listen_port() const override { return std::nullopt; }
};

} // namespace ocudu::ocucp::fuzz
