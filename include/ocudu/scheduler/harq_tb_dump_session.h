// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <memory>
#include <string>

namespace ocudu {

class harq_tb_dumper;

/// \brief [netsys] Keeps the per-TB HARQ statistics dump active during the lifetime of this object.
///
/// The object must be created before the real-time threads, so that the dump threads do not inherit their scheduling
/// priority or CPU affinity, and it must be destroyed after those threads have stopped.
class harq_tb_dump_session
{
public:
  /// \brief Starts the dump, unless the address is empty.
  ///
  /// \param[in] address ZeroMQ address to bind, e.g. "tcp://127.0.0.1:5557". If it is empty, the dump is disabled and
  /// this object does nothing.
  explicit harq_tb_dump_session(const std::string& address);
  ~harq_tb_dump_session();

  harq_tb_dump_session(const harq_tb_dump_session&)            = delete;
  harq_tb_dump_session& operator=(const harq_tb_dump_session&) = delete;

private:
  /// Dumper owned by this session. It is \c nullptr if the dump is disabled.
  std::unique_ptr<harq_tb_dumper> dumper;
};

} // namespace ocudu
