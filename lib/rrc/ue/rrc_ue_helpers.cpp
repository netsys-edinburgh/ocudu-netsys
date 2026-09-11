// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "rrc_ue_helpers.h"
#include "ocudu/asn1/rrc_nr/dl_ccch_msg.h"
#include "ocudu/asn1/rrc_nr/dl_dcch_msg.h"
#include "ocudu/asn1/rrc_nr/ul_ccch_msg.h"
#include "ocudu/asn1/rrc_nr/ul_ccch_msg_ies.h"
#include "ocudu/asn1/rrc_nr/ul_dcch_msg.h"
#include "ocudu/asn1/rrc_nr/ul_dcch_msg_ies.h"
#include <type_traits>

using namespace ocudu;
using namespace ocucp;

namespace {

/// \brief Returns the name of the carried message, which a message class extension holds outside c1.
///
/// Reading c1 of a message class extension only logs an ASN.1 error and then reads the wrong member of the union, so
/// the alternative has to be checked before the name is taken.
template <class T>
const char* get_rrc_message_name(const T& msg)
{
  using msg_type_t = std::decay_t<decltype(msg.msg)>;
  if (msg.msg.type().value != msg_type_t::types_opts::c1) {
    return msg.msg.type().to_string();
  }
  return msg.msg.c1().type().to_string();
}

} // namespace

template <class T>
void ocudu::ocucp::log_rrc_message(rrc_ue_logger&    logger,
                                   const direction_t dir,
                                   byte_buffer_view  pdu,
                                   const T&          msg,
                                   srb_id_t          srb_id,
                                   const char*       msg_type)
{
  if (logger.get_basic_logger().debug.enabled()) {
    asn1::json_writer js;
    msg.to_json(js);
    logger.log_debug(pdu.begin(),
                     pdu.end(),
                     "{} {} {} {} ({} B)",
                     (dir == Rx) ? "Rx" : "Tx",
                     srb_id,
                     msg_type,
                     get_rrc_message_name(msg),
                     pdu.length());
    logger.log_debug("Containerized {}: {}", get_rrc_message_name(msg), js.to_string());
  } else if (logger.get_basic_logger().info.enabled()) {
    std::vector<uint8_t> bytes{pdu.begin(), pdu.end()};
    logger.log_info(pdu.begin(), pdu.end(), "{} {}", msg_type, get_rrc_message_name(msg));
  }
}

template void ocudu::ocucp::log_rrc_message<asn1::rrc_nr::ul_ccch_msg_s>(rrc_ue_logger&                     logger,
                                                                         const direction_t                  dir,
                                                                         byte_buffer_view                   pdu,
                                                                         const asn1::rrc_nr::ul_ccch_msg_s& msg,
                                                                         srb_id_t                           srb_id,
                                                                         const char*                        msg_type);

template void ocudu::ocucp::log_rrc_message<asn1::rrc_nr::ul_dcch_msg_s>(rrc_ue_logger&                     logger,
                                                                         const direction_t                  dir,
                                                                         byte_buffer_view                   pdu,
                                                                         const asn1::rrc_nr::ul_dcch_msg_s& msg,
                                                                         srb_id_t                           srb_id,
                                                                         const char*                        msg_type);

template void ocudu::ocucp::log_rrc_message<asn1::rrc_nr::dl_ccch_msg_s>(rrc_ue_logger&                     logger,
                                                                         const direction_t                  dir,
                                                                         byte_buffer_view                   pdu,
                                                                         const asn1::rrc_nr::dl_ccch_msg_s& msg,
                                                                         srb_id_t                           srb_id,
                                                                         const char*                        msg_type);

template void ocudu::ocucp::log_rrc_message<asn1::rrc_nr::dl_dcch_msg_s>(rrc_ue_logger&                     logger,
                                                                         const direction_t                  dir,
                                                                         byte_buffer_view                   pdu,
                                                                         const asn1::rrc_nr::dl_dcch_msg_s& msg,
                                                                         srb_id_t                           srb_id,
                                                                         const char*                        msg_type);

rrc_ue_capabilities_t ocudu::ocucp::get_capabilities(asn1::rrc_nr::ue_nr_cap_s& ue_capabilities, rrc_ue_logger& logger)
{
  rrc_ue_capabilities_t capabilities{};

  // Set RRC Inactive support.
  if (ue_capabilities.non_crit_ext_present and ue_capabilities.non_crit_ext.inactive_state_present) {
    capabilities.rrc_inactive_supported = true;
    logger.log_debug("RRC Inactive supported by UE");
  }

  // Set CHO support flags. Per TS 38.306 Section 4.2.7.2 each capability must be set consistently
  // across all bands in the same band group, so a single OR across all bands is sufficient.
  for (const auto& band : ue_capabilities.rf_params.supported_band_list_nr) {
    if (band.cond_ho_r16_present) {
      capabilities.conditional_handover_supported = true;
    }
    // TODO: parse condHandoverFailure-r16 (band.cond_ho_fail_r16_present)
    if (band.cond_ho_two_trigger_events_r16_present) {
      capabilities.conditional_handover_two_trigger_events_supported = true;
    }
    if (band.event_a4_based_cond_ho_r17_present) {
      capabilities.conditional_handover_event_a4_supported = true;
    }
    if (band.location_based_cond_ho_r17_present) {
      capabilities.conditional_handover_location_based_supported = true;
    }
    if (band.time_based_cond_ho_r17_present) {
      capabilities.conditional_handover_time_based_supported = true;
    }
  }
  // TODO: parse condHandoverFDD-TDD-r16 and condHandoverFR1-FR2-r16
  if (capabilities.conditional_handover_supported) {
    logger.log_debug("CHO (Rel-16) supported by UE");
  }
  if (capabilities.conditional_handover_two_trigger_events_supported) {
    logger.log_debug("CHO two-trigger-events (Rel-16) supported by UE");
  }
  if (capabilities.conditional_handover_event_a4_supported) {
    logger.log_debug("CHO event-A4-based (Rel-17) supported by UE");
  }
  if (capabilities.conditional_handover_location_based_supported) {
    logger.log_debug("CHO location-based (Rel-17) supported by UE");
  }
  if (capabilities.conditional_handover_time_based_supported) {
    logger.log_debug("CHO time-based (Rel-17) supported by UE");
  }

  return capabilities;
}

std::optional<rrc_ue_capabilities_t>
ocudu::ocucp::get_capabilities(asn1::rrc_nr::ue_cap_rat_container_list_l& capabilities_list, rrc_ue_logger& logger)
{
  // Parse UE capabilities list.
  for (const auto& asn1_ue_capability_rat_container : capabilities_list) {
    if (asn1_ue_capability_rat_container.rat_type != asn1::rrc_nr::rat_type_opts::options::nr) {
      logger.log_debug("Skipping non-NR RAT type in UE capabilities");
      continue;
    }

    asn1::rrc_nr::ue_nr_cap_s asn1_ue_nr_cap;
    {
      asn1::cbit_ref bref(asn1_ue_capability_rat_container.ue_cap_rat_container);
      if (asn1_ue_nr_cap.unpack(bref) != asn1::OCUDUASN_SUCCESS) {
        logger.log_error(asn1_ue_capability_rat_container.ue_cap_rat_container.begin(),
                         asn1_ue_capability_rat_container.ue_cap_rat_container.end(),
                         "Failed to unpack UE Capabilitiy RAT container PDU");
        return std::nullopt;
      }
    }

    return get_capabilities(asn1_ue_nr_cap, logger);
  }

  return rrc_ue_capabilities_t{};
}
