// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// AFL++/libFuzzer harness exercising the full CU-CP NGAP receive path.
///
/// Unlike the ASN.1-only harness (ngap_pdu_decoder_fuzzer), this harness spins
/// up a real CU-CP instance and injects decoded NGAP messages directly into the
/// NGAP message dispatcher.  This exercises the complete receive stack:
///
///   fuzz input -> ASN.1 PER decode -> NGAP validators -> procedure dispatcher
///               -> procedure state-machine -> response generation
///
/// See tests/fuzz/README.md for build instructions, run commands, and the
/// test double architecture (fuzz_amf, fuzz_xnc_gateway).

#include "tests/fuzz/cu_cp/cu_cp_fuzz_env.h"
#include "tests/unittests/ngap/ngap_test_messages.h"
#include "ocudu/adt/byte_buffer.h"
#include "ocudu/adt/mutexed_mpmc_queue.h"
#include "ocudu/asn1/asn1_utils.h"
#include "ocudu/asn1/ngap/common.h"
#include "ocudu/asn1/ngap/ngap.h"
#include "ocudu/asn1/ngap/ngap_ies.h"
#include "ocudu/asn1/ngap/ngap_pdu_contents.h"
#include "ocudu/cu_cp/cu_cp.h"
#include "ocudu/cu_cp/cu_cp_configuration_helpers.h"
#include "ocudu/cu_cp/cu_cp_factory.h"
#include "ocudu/ngap/gateways/n2_connection_client.h"
#include "ocudu/ngap/ngap.h"
#include "ocudu/ngap/ngap_message.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/pcap/dlt_pcap.h"
#include "ocudu/ran/plmn_identity.h"
#include "ocudu/support/async/async_no_op_task.h"
#include "ocudu/support/executors/task_worker.h"
#include "ocudu/support/timers.h"
#include "ocudu/xnap/gateways/xnc_connection_gateway.h"
#include <cstddef>
#include <cstdint>
#include <memory>

using namespace ocudu;
using namespace ocucp;
using namespace ocucp::fuzz;

namespace {

// ---------------------------------------------------------------------------
// Full CU-CP fuzz state
// ---------------------------------------------------------------------------

struct fuzz_state {
  /// Background thread that executes CU-CP tasks.
  task_worker                    worker{"ngap_fuzz_workr", 1024};
  std::unique_ptr<task_executor> exec{std::make_unique<task_worker_executor>(worker)};

  timer_manager    timers{64};
  fuzz_amf         amf;
  fuzz_xnc_gateway xnc_gw;
  null_dlt_pcap    pcap;

  std::unique_ptr<cu_cp> cu_cp_inst;

  fuzz_state()
  {
    // Build CU-CP configuration (same pattern as cu_cp_test_environment).
    cu_cp_configuration cfg = config_helpers::make_default_cu_cp_config();

    cfg.services.cu_cp_executor = exec.get();
    cfg.services.timers         = &timers;

    // Attach our stub AMF as the only NGAP peer.
    s_nssai_t               nssai{slice_service_type{1}, slice_differentiator{}};
    plmn_item               plmn{plmn_identity::test_value(), {nssai}};
    supported_tracking_area ta{7, {plmn}};
    cfg.ngap.n2_gws.push_back(&amf);
    cfg.ngap.ngaps.push_back(cu_cp_configuration::ngap_config{{ta}});

    // Attach our no-op Xn-C gateway.
    cfg.xnap.xnc_gws.push_back(&xnc_gw);

    // Security preferences (NIA2/NEA0 as default).
    cfg.security.int_algo_pref_list = {security::integrity_algorithm::nia2,
                                       security::integrity_algorithm::nia1,
                                       security::integrity_algorithm::nia3,
                                       security::integrity_algorithm::nia0};
    cfg.security.enc_algo_pref_list = {security::ciphering_algorithm::nea0,
                                       security::ciphering_algorithm::nea2,
                                       security::ciphering_algorithm::nea1,
                                       security::ciphering_algorithm::nea3};
    cfg.bearers.drb_config          = config_helpers::make_default_cu_cp_qos_config_list();

    cu_cp_inst = create_cu_cp(cfg);

    // Pre-queue the NGSetupResponse so cu_cp->start() can complete the NG
    // setup handshake without a real AMF.
    amf.enqueue_auto_response(generate_ng_setup_response());

    // Start the CU-CP.  Blocks until the NG Setup procedure completes.
    if (!cu_cp_inst->start()) {
      // Should not happen with the auto-response above; abort to make
      // initialization failures visible during corpus refinement.
      std::abort();
    }

    // Drain the NGSetupRequest that the CU-CP sent.
    ngap_message dummy;
    while (amf.try_pop_rx_pdu(dummy)) {
    }
    worker.wait_pending_tasks();
  }
};

// ---------------------------------------------------------------------------
// Global state – lazy, created inside the (post-fork) child process so that
// the task_worker thread is never inherited across a fork().
//
// The state is deliberately leaked: the ocudulog singleton registers its exit handler before the
// state exists, so it is torn down first and stopping the CU-CP at exit would log through a freed
// logger. The OS reclaims everything when the process ends.
// ---------------------------------------------------------------------------

static fuzz_state*    g_state = nullptr;
static std::once_flag g_init_flag;

/// Number of 1ms timer ticks executed after each input.
///
/// Advancing the clock lets timers armed while handling the input fire. Kept small on purpose: each
/// tick costs throughput, and the long CU-CP guard timers are out of reach at this granularity by
/// design.
constexpr unsigned nof_timer_ticks_per_input = 8;

static void ensure_state()
{
  std::call_once(g_init_flag, []() { g_state = new fuzz_state(); });
}

} // namespace

// ---------------------------------------------------------------------------
// LLVMFuzzer interface
// ---------------------------------------------------------------------------

/// Suppress log noise for all CU-CP subsystems before any forking occurs.
extern "C" int LLVMFuzzerInitialize(int* /*argc*/, char*** /*argv*/)
{
  for (const char* name : {"NGAP", "CU-CP", "RRC", "PDCP", "SEC", "F1AP", "E1AP", "NRPPA", "XNAP", "ALL"}) {
    ocudulog::fetch_basic_logger(name).set_level(ocudulog::basic_levels::debug);
  }
  // NOTE: do NOT call ensure_state() here.  The state (and its task_worker
  // thread) must be created after AFL++ forks so each child has its own thread.
  return 0;
}

/// Entry point called by AFL++ / libFuzzer for each mutated input.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
  // Lazy initialisation: safe after AFL++ fork, called once per process with
  // libFuzzer and once per child-process lifecycle with AFL++.
  ensure_state();

  // ------------------------------------------------------------------
  // Step 1 – decode the fuzz input into an ngap_message.
  //
  // Inputs that fail to decode are dropped, mirroring ngap_asn1_packer::handle_packed_pdu(): the
  // CU-CP never sees them in production, so injecting them would only exercise unreachable states.
  // The ASN.1 layer itself is covered by ngap_pdu_decoder_fuzzer.
  // ------------------------------------------------------------------
  byte_buffer    buf{byte_buffer::fallback_allocation_tag{}, span<const uint8_t>(data, size)};
  asn1::cbit_ref bref{buf};
  ngap_message   msg{};
  if (msg.pdu.unpack(bref) != asn1::OCUDUASN_SUCCESS) {
    return 0;
  }

  // ------------------------------------------------------------------
  // Step 2 – inject the message into the CU-CP via the AMF stub.
  // ------------------------------------------------------------------
  g_state->amf.push_tx_pdu(msg);

  // ------------------------------------------------------------------
  // Step 3 – drain the CU-CP task queue so all async procedures run.
  //
  // Coroutine continuations may queue further tasks; two rounds of
  // wait_pending_tasks() covers procedures that span a single await point.
  // ------------------------------------------------------------------
  g_state->worker.wait_pending_tasks();
  g_state->worker.wait_pending_tasks();

  // ------------------------------------------------------------------
  // Step 4 – advance the timer to exercise the expiry paths of the timers armed while handling this
  // input.
  // ------------------------------------------------------------------
  for (unsigned i = 0; i != nof_timer_ticks_per_input; ++i) {
    g_state->worker.push_task_blocking([&]() { g_state->timers.tick(); });
    g_state->worker.wait_pending_tasks();
  }

  // ------------------------------------------------------------------
  // Step 5 – drain CU-CP responses to prevent queue overflow across
  // iterations.
  // ------------------------------------------------------------------
  ngap_message response;
  while (g_state->amf.try_pop_rx_pdu(response)) {
  }

  return 0;
}
