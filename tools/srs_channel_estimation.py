#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Pluggable SRS channel estimation for occasions dumped by srs_iq_dump_consumer.py.

Provides a small estimator registry (`get_estimator`, `list_estimators`) so alternative algorithms can be added
alongside the default Least Squares (LS) estimator, without changing any caller code.

Known limitations of the current LS estimator - see its class docstring for details:
  - Only the TS 38.211 "long sequence" low-PAPR reference (SRS bandwidth >= 36 subcarriers) is implemented; smaller
    occasions raise NotImplementedError rather than silently producing a wrong estimate.
  - Assumes SRS group/sequence hopping is disabled (this information isn't in the dump header at all).
  - Assumes the captured buffer's subcarrier index 0 aligns with the SRS resource's own reference point; adjust
    `k_grid_offset` if your deployment's resource grid has an additional carrier offset.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass

import numpy as np

# Number of subcarriers per resource block.
NOF_SUBCARRIERS_PER_RB = 12

# TS 38.211 Table 6.4.1.4.3-1, b_srs=0 column only (m_srs,0, i.e. the SRS bandwidth in RBs for a single, non-hopping
# occasion). Ported from lib/ran/srs/srs_bandwidth_configuration.cpp (table0) to keep this in sync with ocudu's own
# implementation, rather than re-transcribing the spec table by hand. Indexed by configuration_index (C_SRS, 0..63).
_SRS_BANDWIDTH_TABLE_B0_M_SRS = [
    4, 8, 12, 16, 16, 20, 24, 24, 28, 32, 36, 40, 48, 48, 52, 56, 60, 64, 72, 72, 76, 80, 88, 96, 96, 104, 112, 120,
    120, 120, 128, 128, 128, 132, 136, 144, 144, 144, 144, 152, 160, 160, 160, 168, 176, 184, 192, 192, 192, 192,
    208, 216, 224, 240, 240, 240, 240, 256, 256, 256, 264, 272, 272, 272,
]


def _is_prime(n: int) -> bool:
    if n < 2:
        return False
    if n % 2 == 0:
        return n == 2
    for d in range(3, int(n**0.5) + 1, 2):
        if n % d == 0:
            return False
    return True


def _largest_prime_below(n: int) -> int:
    for candidate in range(n - 1, 1, -1):
        if _is_prime(candidate):
            return candidate
    raise ValueError(f"No prime found below {n}")


def srs_occasion_bandwidth_subc(header: dict) -> int:
    """Returns M_sc_RS: the number of SRS-carrying subcarriers in the occasion (after comb decimation)."""
    m_srs = _SRS_BANDWIDTH_TABLE_B0_M_SRS[header["configuration_index"]]
    return m_srs * NOF_SUBCARRIERS_PER_RB // header["comb_size"]


def generate_srs_reference_sequence(header: dict) -> np.ndarray:
    """Generates the SRS low-PAPR reference sequence r(n), n = 0..M_sc_RS-1, for the occasion described by header.

    Implements the TS 38.211 5.2.2 "long sequence" (Zadoff-Chu derived) low-PAPR sequence, as used for M_sc_RS >=
    36. Assumes group/sequence hopping is disabled (u = sequence_id mod 30, v = 0) - see module docstring.

    Raises:
        NotImplementedError: if M_sc_RS < 36 (the short-sequence, lookup-table-based case, not implemented here).
    """
    m_sc_rs = srs_occasion_bandwidth_subc(header)
    # ocudu's own generator (low_papr_sequence_generator_impl::r_uv_arg) switches from lookup-table-based short
    # sequences to this same ZC-derived formula at M_zc >= 36 - verified by reading
    # lib/phy/upper/sequence_generators/low_papr_sequence_generator_impl.cpp.
    if m_sc_rs < 36:
        raise NotImplementedError(
            f"SRS occasion bandwidth ({m_sc_rs} subcarriers) is below 36; the short-sequence (lookup-table-based) "
            f"reference generation is not implemented here. See the module docstring for why."
        )

    u = header["sequence_id"] % 30
    v = 0
    n_zc = _largest_prime_below(m_sc_rs)

    q_bar = n_zc * (u + 1) / 31
    q = np.floor(q_bar + 0.5) + v * (-1) ** np.floor(2 * q_bar)

    m = np.arange(n_zc)
    x_q = np.exp(-1j * np.pi * q * m * (m + 1) / n_zc)

    # Cyclically extend the length-N_ZC base sequence to the occasion's length M_sc_RS.
    r_bar = x_q[np.arange(m_sc_rs) % n_zc]

    # Apply the resource's cyclic shift. n_cs_max is 8 for comb size 2, 12 for comb size 4 (TS 38.211 6.4.1.4.2).
    n_cs_max = 8 if header["comb_size"] == 2 else 12
    alpha = 2 * np.pi * header["cyclic_shift"] / n_cs_max
    n = np.arange(m_sc_rs)

    return np.exp(1j * alpha * n) * r_bar


def srs_occasion_subcarrier_indices(header: dict, k_grid_offset: int = 0) -> np.ndarray:
    """Returns the absolute subcarrier indices (into the captured IQ buffer) carrying SRS, for this occasion."""
    m_sc_rs = srs_occasion_bandwidth_subc(header)
    k0 = header["freq_shift"] * NOF_SUBCARRIERS_PER_RB + k_grid_offset
    return k0 + header["comb_offset"] + header["comb_size"] * np.arange(m_sc_rs)


@dataclass
class ChannelEstimateResult:
    """Result of a channel estimate: complex gain per (symbol, port, SRS-carrying subcarrier)."""

    # Shape (nof_symbols, nof_ports, M_sc_RS).
    h: np.ndarray
    # Absolute subcarrier index (into the captured buffer) for each entry of h's last axis. Shape (M_sc_RS,).
    subcarrier_indices: np.ndarray
    # Name of the estimator that produced this result.
    estimator_name: str


class ChannelEstimator(ABC):
    """Base class for pluggable SRS channel estimators. Register new implementations with @register_estimator."""

    name: str = "base"

    def __init__(self, k_grid_offset: int = 0):
        """\
        Args:
            k_grid_offset: Extra subcarrier offset added to the SRS resource's own reference point, in case the
                captured buffer's index 0 doesn't align with subcarrier 0 of that reference point in your
                deployment. Defaults to 0 (buffer index 0 == the resource's reference point).
        """
        self.k_grid_offset = k_grid_offset

    @abstractmethod
    def estimate(self, iq: np.ndarray, header: dict) -> ChannelEstimateResult:
        """Estimates the channel from one occasion's IQ samples (shape [nof_symbols, nof_ports, nof_subc]) and
        its header metadata."""


_REGISTRY: dict = {}


def register_estimator(cls):
    """Class decorator that registers a ChannelEstimator subclass under its `name`."""
    _REGISTRY[cls.name] = cls
    return cls


def list_estimators() -> list:
    return sorted(_REGISTRY)


def get_estimator(name: str, **kwargs) -> ChannelEstimator:
    if name not in _REGISTRY:
        raise KeyError(f"Unknown channel estimator '{name}'. Available: {list_estimators()}")
    return _REGISTRY[name](**kwargs)


@register_estimator
class LSChannelEstimator(ChannelEstimator):
    """Least Squares channel estimator: H(k) = Y(k) / X(k) at each SRS-carrying subcarrier k.

    See the module docstring for the assumptions/limitations (long-sequence only, no hopping, grid alignment).
    """

    name = "ls"

    def estimate(self, iq: np.ndarray, header: dict) -> ChannelEstimateResult:
        r = generate_srs_reference_sequence(header)
        k_indices = srs_occasion_subcarrier_indices(header, self.k_grid_offset)

        nof_subc = iq.shape[2]
        if k_indices[-1] >= nof_subc or k_indices[0] < 0:
            raise ValueError(
                f"SRS occasion subcarriers [{k_indices[0]}, {k_indices[-1]}] fall outside the captured buffer "
                f"width (0, {nof_subc}); check k_grid_offset."
            )

        y = iq[:, :, k_indices]
        h = y / r[np.newaxis, np.newaxis, :]
        return ChannelEstimateResult(h=h, subcarrier_indices=k_indices, estimator_name=self.name)
