// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <memory>
#include <string>

namespace ocudu {

class rlc_sdu_dumper;

/// \brief [netsys] Keeps the per-SDU RLC AM reTx statistics dump active during the lifetime of this object.
///
/// The object must be created before the real-time threads, so that the dump threads do not inherit their scheduling
/// priority or CPU affinity, and it must be destroyed after those threads have stopped.
class rlc_sdu_dump_session
{
public:
  /// \brief Starts the dump, unless the address is empty.
  ///
  /// \param[in] address ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5556". If it is empty, the dump is disabled and
  /// this object does nothing.
  explicit rlc_sdu_dump_session(const std::string& address);
  ~rlc_sdu_dump_session();

  rlc_sdu_dump_session(const rlc_sdu_dump_session&)            = delete;
  rlc_sdu_dump_session& operator=(const rlc_sdu_dump_session&) = delete;

private:
  /// Dumper owned by this session. It is \c nullptr if the dump is disabled.
  std::unique_ptr<rlc_sdu_dumper> dumper;
};

} // namespace ocudu
