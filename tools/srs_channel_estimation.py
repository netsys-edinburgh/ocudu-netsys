#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Pluggable SRS channel estimation for occasions dumped by srs_iq_dump_consumer.py.

Provides a small estimator registry (`get_estimator`, `list_estimators`) so alternative algorithms can be added
alongside the default Least Squares (LS) estimator, without changing any caller code.

The dump itself already contains only the occasion's own SRS-carrying REs (M_sc_RS of them, densely packed, no
comb gaps) - the gNB-side plugin extracts them via the same `get_srs_information()` helper ocudu's own SRS channel
estimator uses, and reports the exact bandwidth (`nof_subc`) and first subcarrier (`mapping_initial_subcarrier`) it
used in the dump header. So `M_sc_RS` and the REs' absolute grid position are read straight from the header rather
than re-derived here.

Known limitations of the current LS estimator - see its class docstring for details:
  - Only the TS 38.211 "long sequence" low-PAPR reference (SRS bandwidth >= 36 subcarriers) is implemented; smaller
    occasions raise NotImplementedError rather than silently producing a wrong estimate.
  - Assumes SRS group/sequence hopping is disabled (this information isn't in the dump header at all) - matching
    the same assumption ocudu's own `get_srs_information()` hard-asserts on.
"""

from abc import ABC, abstractmethod
from dataclasses import dataclass

import numpy as np


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


def generate_srs_reference_sequence(header: dict) -> np.ndarray:
    """Generates the SRS low-PAPR reference sequence r(n), n = 0..M_sc_RS-1, for the occasion described by header.

    Implements the TS 38.211 5.2.2 "long sequence" (Zadoff-Chu derived) low-PAPR sequence, as used for M_sc_RS >=
    36. Assumes group/sequence hopping is disabled (u = sequence_id mod 30, v = 0) - see module docstring.

    Raises:
        NotImplementedError: if M_sc_RS < 36 (the short-sequence, lookup-table-based case, not implemented here).
    """
    m_sc_rs = header["nof_subc"]
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
    """Returns the absolute (resource grid) subcarrier index for each RE captured in the IQ buffer.

    The buffer itself already holds only these REs, densely packed (buffer index n is absolute subcarrier
    `mapping_initial_subcarrier + n * comb_size`) - this only labels that axis, e.g. for plotting; it does not
    select or reorder anything. `k_grid_offset` shifts the label only, in case you want it to read out relative to
    a different reference point than the one `mapping_initial_subcarrier` already uses.
    """
    m_sc_rs = header["nof_subc"]
    k0 = header["mapping_initial_subcarrier"] + k_grid_offset
    return k0 + header["comb_size"] * np.arange(m_sc_rs)


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
            k_grid_offset: Extra offset added when labeling REs with their absolute subcarrier index (see
                `srs_occasion_subcarrier_indices`). Purely cosmetic - the dump already contains only this
                occasion's own REs, so this does not affect which samples are used or how the estimate is
                computed. Defaults to 0.
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

        # The buffer already contains exactly the occasion's M_sc_RS REs, densely packed - no slicing needed.
        nof_subc = iq.shape[2]
        if nof_subc != len(r):
            raise ValueError(
                f"IQ buffer has {nof_subc} subcarriers but the occasion's reference sequence has {len(r)} "
                f"(header['nof_subc']={header['nof_subc']}); header/data mismatch?"
            )

        h = iq / r[np.newaxis, np.newaxis, :]
        return ChannelEstimateResult(h=h, subcarrier_indices=k_indices, estimator_name=self.name)
