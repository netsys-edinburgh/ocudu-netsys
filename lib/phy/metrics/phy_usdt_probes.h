// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

/// USDT (User Statically Defined Tracing) probes for the upper PHY.
///
/// Probes are zero-cost when no tracer is attached:
///   - With OCUDU_USDT_ENABLED: a single nop instruction per probe site; args remain in registers
///   - Without OCUDU_USDT_ENABLED: macros expand to nothing; args are never evaluated
///
/// Enable at build time with -DENABLE_USDT=ON (sets OCUDU_USDT_ENABLED).
///
/// Attaching probes with bpftrace:
///   bpftrace -e 'usdt:./ocudu:ocudu_phy:dl_slot_send { @[hist(arg2/1000)] = count(); }'
///
/// Provider:  ocudu_phy
/// Probes:
///   dl_slot_start    (sfn, slot_idx)
///   dl_slot_send     (sfn, slot_idx, elapsed_finish_ns, elapsed_configure_ns)
///   pdsch_start      (sfn, slot_idx, rnti, tbs_bytes)
///   pdsch_done       (sfn, slot_idx, rnti, elapsed_return_ns, elapsed_completion_ns)
///   pusch_start      (sfn, slot_idx, rnti, tbs_bytes)
///   pusch_done       (sfn, slot_idx, rnti, elapsed_data_ns, crc_ok)
///   ldpc_decode_start(cb_size_bits)
///   ldpc_decode_done (cb_size_bits, nof_iterations, crc_ok, elapsed_ns)

#ifdef OCUDU_USDT_ENABLED
#include <sys/sdt.h>
// clang-format off
#define OCUDU_PHY_PROBE2(name, a, b)             DTRACE_PROBE2(ocudu_phy, name, a, b)
#define OCUDU_PHY_PROBE4(name, a, b, c, d)       DTRACE_PROBE4(ocudu_phy, name, a, b, c, d)
#define OCUDU_PHY_PROBE5(name, a, b, c, d, e)    DTRACE_PROBE5(ocudu_phy, name, a, b, c, d, e)
#define OCUDU_PHY_PROBE1(name, a)                DTRACE_PROBE1(ocudu_phy, name, a)
// clang-format on
#else
#define OCUDU_PHY_PROBE1(name, a)                do {} while (0)
#define OCUDU_PHY_PROBE2(name, a, b)             do {} while (0)
#define OCUDU_PHY_PROBE4(name, a, b, c, d)       do {} while (0)
#define OCUDU_PHY_PROBE5(name, a, b, c, d, e)    do {} while (0)
#endif
