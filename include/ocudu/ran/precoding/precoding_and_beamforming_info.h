// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/static_vector.h"
#include "ocudu/ran/beamforming/beam_identifier.h"
#include "ocudu/ran/precoding/precoding_constants.h"
#include "ocudu/ran/precoding/precoding_matrix_indicator.h"

namespace ocudu {

/// Precoding and beamforming applied to one Precoding Resource Block Group (PRG).
struct prg_precoding_and_beamforming {
  /// \brief Precoding Matrix Indicator that maps the transmission layers onto the precoding matrix ports.
  ///
  /// \c std::monostate selects no precoding, i.e. an identity matrix that maps each layer onto the port with the same
  /// index.
  precoding_matrix_indicator pmi;
  /// \brief Beam that carries each port of the precoding matrix.
  ///
  /// An empty list selects the default beams, i.e. the beams that map directly onto the antenna ports, as given by
  /// \ref get_default_beam_list. Otherwise it holds one beam per port of the precoding matrix.
  precoding_beam_list beams;
};

/// \brief Precoding and beamforming information of a downlink transmission.
struct precoding_and_beamforming_info {
  /// \brief Value of \ref nof_rbs_per_prg that marks a wideband transmission.
  ///
  /// The single PRG spans the complete allocation, whose size is resolved by the consumer of this structure. It is the
  /// consumer, and not the producer, that knows the allocation in the units that the receiving interface expects.
  static constexpr unsigned wideband_prg = 0;

  /// \brief Size in RBs of a PRG, i.e. the RBs that share the same precoding and beamforming.
  ///
  /// Possible values: {1, ..., 275}, or \ref wideband_prg.
  unsigned nof_rbs_per_prg = wideband_prg;
  /// PRG list, in ascending resource block order.
  static_vector<prg_precoding_and_beamforming, precoding_constants::MAX_NOF_PRG> prgs;

  /// Returns true if the same precoding and beamforming applies to the complete allocation.
  bool is_wideband() const { return nof_rbs_per_prg == wideband_prg; }
};

/// \brief Builds the precoding and beamforming of a transmission that is mapped onto a single beam.
///
/// The transmission is not precoded, and the complete allocation is carried by the given beam.
inline precoding_and_beamforming_info make_single_beam_precoding(beam_identifier beam_id)
{
  precoding_and_beamforming_info info;
  info.prgs.push_back(prg_precoding_and_beamforming{.pmi = {}, .beams = {beam_id}});

  return info;
}

/// \brief Builds the precoding and beamforming of a transmission that uses the default beams.
///
/// The transmission is neither precoded nor beamformed: each layer is mapped onto the antenna port with the same index.
inline precoding_and_beamforming_info make_default_precoding()
{
  precoding_and_beamforming_info info;
  info.prgs.emplace_back();

  return info;
}

} // namespace ocudu
