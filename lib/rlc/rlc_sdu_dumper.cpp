// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "rlc_sdu_dumper.h"
#include "ocudu/rlc/rlc_sdu_dump_session.h"
#include "fmt/format.h"

using namespace ocudu;

/// Dumper in use, or \c nullptr if the dump is disabled.
static std::atomic<rlc_sdu_dumper*> active_dumper{nullptr};

rlc_sdu_dump_session::rlc_sdu_dump_session(const std::string& address)
{
  if (address.empty()) {
    return;
  }

  dumper = std::make_unique<rlc_sdu_dumper>(address);
  dumper->start();
  active_dumper.store(dumper.get(), std::memory_order_release);
  fmt::println("RLC SDU dump: streaming to {}", address);
}

rlc_sdu_dump_session::~rlc_sdu_dump_session()
{
  // The member dumper is destroyed after this line, which sends the remaining records and closes the socket.
  active_dumper.store(nullptr, std::memory_order_release);
}

rlc_sdu_dumper* rlc_sdu_dumper::get() noexcept
{
  return active_dumper.load(std::memory_order_acquire);
}
