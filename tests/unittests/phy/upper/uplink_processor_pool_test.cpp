// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "../../../lib/phy/upper/uplink_processor_pool_impl.h"
#include "uplink_processor_test_doubles.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

/// Builds a pool with \c nof_processors spies for 30 kHz SCS and returns non-owning pointers to them.
std::unique_ptr<uplink_processor_pool_impl> make_pool(unsigned                            nof_processors,
                                                      std::vector<uplink_processor_spy*>& spies,
                                                      uplink_processor_spy*&              default_spy)
{
  uplink_processor_pool_impl_config config;
  auto&                             set = config.procs.emplace_back();
  set.scs                               = subcarrier_spacing::kHz30;
  for (unsigned i = 0; i != nof_processors; ++i) {
    auto proc = std::make_unique<uplink_processor_spy>();
    spies.push_back(proc.get());
    set.procs.emplace_back(std::move(proc));
  }
  auto def                 = std::make_unique<uplink_processor_spy>();
  default_spy              = def.get();
  config.default_processor = std::move(def);
  return std::make_unique<uplink_processor_pool_impl>(std::move(config));
}

} // namespace

// UL_TTI.requests arrive max_proc_delay slots ahead of the air time, and on a split-8 radio the slot indications run
// ahead of the air time by the transmit lead as well. Receive symbols of a slot are therefore looked up while
// requests for slots well beyond it have already been assigned. The lookup must still reach the processor that was
// configured for that slot: a routing table that wraps inside that span hands the symbols to the processor of a later
// slot, which ignores them, and the slot's PDUs are only discarded when the processor is reused.
TEST(uplink_processor_pool, rx_symbols_reach_their_slot_with_requests_32_slots_ahead)
{
  std::vector<uplink_processor_spy*> spies;
  uplink_processor_spy*              default_spy = nullptr;
  constexpr unsigned                 nof_processors = 80;
  constexpr unsigned                 lead_slots     = 32;
  auto                               pool           = make_pool(nof_processors, spies, default_spy);

  uplink_pdu_slot_repository_pool& repositories = pool->get_slot_pdu_repository_pool();
  uplink_slot_processor_pool&      rx_pool      = pool->get_slot_processor_pool();

  // Assign one processor per slot, in order, as the FAPI translator does for UL_TTI.requests with PDUs. The pool hands
  // processors out round-robin, so the processor for slot n is spies[n % nof_processors].
  constexpr unsigned first_slot = 20470; // Crosses the SFN wrap at 1024 frames x 20 slots.
  constexpr unsigned wrap       = 1024 * 20;
  for (unsigned i = 0; i != 200; ++i) {
    slot_point request_slot(subcarrier_spacing::kHz30, (first_slot + i) % wrap);
    (void)repositories.get_pdu_slot_repository(request_slot);

    if (i < lead_slots) {
      continue;
    }
    // Symbols of the slot that is lead_slots behind the newest request.
    unsigned               rx_index = i - lead_slots;
    slot_point             rx_slot(subcarrier_spacing::kHz30, (first_slot + rx_index) % wrap);
    uplink_slot_processor& routed   = rx_pool.get_slot_processor(rx_slot);
    uplink_slot_processor& expected = spies[rx_index % nof_processors]->get_slot_processor(rx_slot);
    ASSERT_EQ(&routed, &expected) << "rx slot " << rx_slot.to_uint() << " routed to another processor with requests up to "
                                  << request_slot.to_uint();
  }
}
