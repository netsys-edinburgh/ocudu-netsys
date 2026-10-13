// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "f1c_aurora_tap.h"
#include "f1ap_asn1_utils.h"
#include "ocudu/asn1/f1ap/common.h"
#include "ocudu/asn1/f1ap/f1ap.h"

using namespace ocudu;
using namespace asn1::f1ap;

namespace {

aurora::f1c_proc to_aurora_proc(uint16_t proc_code)
{
  switch (proc_code) {
    case ASN1_F1AP_ID_F1_SETUP:
      return aurora::f1c_proc::f1_setup;
    case ASN1_F1AP_ID_UE_CONTEXT_SETUP:
      return aurora::f1c_proc::ue_ctx_setup;
    case ASN1_F1AP_ID_UE_CONTEXT_MOD:
      return aurora::f1c_proc::ue_ctx_mod;
    case ASN1_F1AP_ID_UE_CONTEXT_RELEASE:
      return aurora::f1c_proc::ue_ctx_release;
    case ASN1_F1AP_ID_UE_CONTEXT_RELEASE_REQUEST:
      return aurora::f1c_proc::ue_ctx_release_req;
    case ASN1_F1AP_ID_INIT_UL_RRC_MSG_TRANSFER:
      return aurora::f1c_proc::init_ul_rrc;
    case ASN1_F1AP_ID_UL_RRC_MSG_TRANSFER:
      return aurora::f1c_proc::ul_rrc;
    case ASN1_F1AP_ID_DL_RRC_MSG_TRANSFER:
      return aurora::f1c_proc::dl_rrc;
    case ASN1_F1AP_ID_PAGING:
      return aurora::f1c_proc::paging;
    case ASN1_F1AP_ID_RESET:
      return aurora::f1c_proc::reset;
    case ASN1_F1AP_ID_ERROR_IND:
      return aurora::f1c_proc::error_indication;
    default:
      return aurora::f1c_proc::other;
  }
}

/// Request/response matching key: the gNB-CU UE F1AP ID of UE-associated procedures, the transaction ID otherwise.
std::optional<uint64_t> request_key(const f1ap_message& msg, aurora::f1c_proc proc)
{
  if (proc == aurora::f1c_proc::ue_ctx_setup || proc == aurora::f1c_proc::ue_ctx_mod ||
      proc == aurora::f1c_proc::ue_ctx_release) {
    if (std::optional<gnb_cu_ue_f1ap_id_t> id = get_gnb_cu_ue_f1ap_id(msg.pdu)) {
      return static_cast<uint64_t>(*id);
    }
    return std::nullopt;
  }
  if (std::optional<uint8_t> id = get_transaction_id(msg.pdu)) {
    return uint64_t{*id};
  }
  return std::nullopt;
}

uint32_t encoded_size(const f1ap_message& msg)
{
  byte_buffer   buf{byte_buffer::fallback_allocation_tag{}};
  asn1::bit_ref bref(buf);
  if (msg.pdu.pack(bref) != asn1::OCUDUASN_SUCCESS) {
    return 0;
  }
  return static_cast<uint32_t>(buf.length());
}

bool is_f1_setup_response(const f1ap_message& msg)
{
  return msg.pdu.type().value == f1ap_pdu_c::types_opts::successful_outcome &&
         msg.pdu.successful_outcome().proc_code == ASN1_F1AP_ID_F1_SETUP;
}

} // namespace

void f1c_aurora::record_message(const f1ap_message& msg, aurora::f1c_dir dir, std::optional<uint32_t> nof_bytes)
{
  if (!aurora::is_active()) {
    return;
  }

  aurora::f1c_msg_info info;
  info.dir = dir;
  switch (msg.pdu.type().value) {
    case f1ap_pdu_c::types_opts::init_msg:
      info.outcome = aurora::f1c_outcome::initiating;
      info.proc    = to_aurora_proc(msg.pdu.init_msg().proc_code);
      break;
    case f1ap_pdu_c::types_opts::successful_outcome:
      info.outcome = aurora::f1c_outcome::successful;
      info.proc    = to_aurora_proc(msg.pdu.successful_outcome().proc_code);
      break;
    case f1ap_pdu_c::types_opts::unsuccessful_outcome:
      info.outcome = aurora::f1c_outcome::unsuccessful;
      info.proc    = to_aurora_proc(msg.pdu.unsuccessful_outcome().proc_code);
      break;
    default:
      return;
  }
  info.key       = request_key(msg, info.proc);
  info.nof_bytes = nof_bytes ? *nof_bytes : encoded_size(msg);
  aurora::on_f1c_message(info);
}

void f1c_aurora::record_decode_error(aurora::f1c_dir dir)
{
  aurora::on_f1c_decode_error(dir);
}

f1c_aurora::link_tracker::~link_tracker()
{
  if (up) {
    aurora::on_f1c_link_down();
  }
}

void f1c_aurora::link_tracker::on_dl_message(const f1ap_message& msg)
{
  if (!up && is_f1_setup_response(msg)) {
    up = true;
    aurora::on_f1c_link_up();
  }
}
