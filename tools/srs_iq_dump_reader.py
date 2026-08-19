#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Reads back SRS occasion IQ dumps saved by srs_iq_dump_consumer.py.

Each occasion is a pair of files: `<base>.npy` (complex64 array, shape [nof_symbols, nof_ports, nof_subc]) and
`<base>.json` (the occasion's header metadata). `nof_subc` only covers the occasion's own SRS-carrying subcarriers
(M_sc_RS, after comb decimation) - the array holds just that one UE's resource elements, not the whole resource
grid. This script loads one or more of them, prints a summary, and can optionally plot the average per-RE power (a
quick way to see where in frequency the SRS actually landed, and how flat the channel looks across it).

Usage:
    # Summarize every occasion in a directory:
    python3 srs_iq_dump_reader.py /tmp/srs_dump

    # Summarize and plot a single occasion:
    python3 srs_iq_dump_reader.py /tmp/srs_dump/srs_..._000000.npy --plot

    # Also compute and print a channel estimate (see srs_channel_estimation.py for algorithm details/limitations):
    python3 srs_iq_dump_reader.py /tmp/srs_dump/srs_..._000000.npy --estimate-channel ls --plot
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np

import srs_channel_estimation as sce


def load_occasion(npy_path: Path) -> tuple[np.ndarray, dict]:
    """Loads one occasion's IQ array and its associated header metadata."""
    iq = np.load(npy_path)
    json_path = npy_path.with_suffix(".json")
    header = json.loads(json_path.read_text()) if json_path.exists() else {}
    return iq, header


def summarize(npy_path: Path, iq: np.ndarray, header: dict) -> None:
    print(f"{npy_path.name}")
    if header:
        print(
            f"  rnti={header['rnti']:#06x} sfn={header['sfn']} slot={header['slot_index']} "
            f"symb=[{header['start_symbol']}, {header['start_symbol'] + header['nof_symbols']}) "
            f"scs={header['scs_khz']}kHz comb={header['comb_size']}/{header['comb_offset']} "
            f"cyclic_shift={header['cyclic_shift']} sequence_id={header['sequence_id']} "
            f"config_index={header['configuration_index']} bandwidth_index={header['bandwidth_index']} "
            f"k0={header['mapping_initial_subcarrier']}"
        )
    else:
        print("  (no matching .json sidecar found - showing array info only)")

    print(f"  shape={iq.shape} (nof_symbols, nof_ports, nof_subc), dtype={iq.dtype}")

    power = np.abs(iq) ** 2
    mean_power_db = 10 * np.log10(np.mean(power) + 1e-20)
    peak_power_db = 10 * np.log10(np.max(power) + 1e-20)
    print(f"  mean power={mean_power_db:.1f} dB, peak power={peak_power_db:.1f} dB (arbitrary units)")


def summarize_channel_estimate(result: sce.ChannelEstimateResult) -> None:
    nof_symbols, nof_ports, nof_re = result.h.shape
    print(
        f"  Channel estimate [{result.estimator_name}]: {nof_re} REs "
        f"(subcarriers {result.subcarrier_indices[0]}..{result.subcarrier_indices[-1]})"
    )
    for i_port in range(nof_ports):
        h_port = result.h[:, i_port, :]
        mean_mag_db = 20 * np.log10(np.mean(np.abs(h_port)) + 1e-20)
        std_mag_db = 20 * np.log10(np.std(np.abs(h_port)) + 1e-20)
        print(f"    port {i_port}: mean |H|={mean_mag_db:.1f} dB, std |H|={std_mag_db:.1f} dB")


def plot_channel_estimate(npy_path: Path, result: sce.ChannelEstimateResult, header: dict) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("  matplotlib is not installed (pip install matplotlib); skipping plot.", file=sys.stderr)
        return

    # Average, across symbols, magnitude (dB) and phase per SRS-carrying subcarrier, one line per port.
    mag_db = 20 * np.log10(np.mean(np.abs(result.h), axis=0) + 1e-20)
    phase = np.angle(np.mean(result.h, axis=0))

    fig, (ax_mag, ax_phase) = plt.subplots(2, 1, sharex=True)
    for i_port in range(result.h.shape[1]):
        ax_mag.plot(result.subcarrier_indices, mag_db[i_port], label=f"port {i_port}")
        ax_phase.plot(result.subcarrier_indices, phase[i_port], label=f"port {i_port}")

    ax_mag.set_ylabel("|H| (dB)")
    ax_mag.legend()
    ax_mag.grid(True)
    ax_phase.set_ylabel("phase(H) (rad)")
    ax_phase.set_xlabel("Subcarrier index")
    ax_phase.grid(True)

    title = npy_path.stem
    if header:
        title = f"Channel estimate [{result.estimator_name}] rnti={header['rnti']:#06x} slot={header['slot_index']}"
    fig.suptitle(title)
    fig.tight_layout()
    plt.show()


def plot_occasion(npy_path: Path, iq: np.ndarray, header: dict) -> None:
    try:
        import matplotlib.pyplot as plt
    except ImportError:
        print("  matplotlib is not installed (pip install matplotlib); skipping plot.", file=sys.stderr)
        return

    # Average power per SRS-carrying RE, across symbols and ports, in dB.
    power_db = 10 * np.log10(np.mean(np.abs(iq) ** 2, axis=(0, 1)) + 1e-20)

    plt.figure()
    plt.plot(power_db)
    plt.xlabel("SRS RE index (within the captured occasion, not an absolute subcarrier index)")
    plt.ylabel("Average power (dB)")
    title = npy_path.stem
    if header:
        title = f"rnti={header['rnti']:#06x} sfn={header['sfn']} slot={header['slot_index']}"
    plt.title(title)
    plt.grid(True)
    plt.tight_layout()
    plt.show()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("path", help="A .npy file, or a directory containing dumped .npy files")
    parser.add_argument("--plot", action="store_true", help="Plot average per-subcarrier power for each occasion")
    parser.add_argument(
        "--estimate-channel",
        choices=sce.list_estimators(),
        default=None,
        help="Also compute a channel estimate using the named algorithm (requires the .json sidecar)",
    )
    parser.add_argument(
        "--k-grid-offset",
        type=int,
        default=0,
        help="Extra offset applied only to the printed/plotted absolute subcarrier index (cosmetic; see "
        "srs_channel_estimation.py)",
    )
    args = parser.parse_args()

    path = Path(args.path)
    if path.is_dir():
        npy_files = sorted(path.glob("*.npy"))
        if not npy_files:
            print(f"No .npy files found in {path}", file=sys.stderr)
            return 1
    elif path.is_file():
        npy_files = [path]
    else:
        print(f"Path not found: {path}", file=sys.stderr)
        return 1

    for npy_path in npy_files:
        iq, header = load_occasion(npy_path)
        summarize(npy_path, iq, header)
        if args.plot:
            plot_occasion(npy_path, iq, header)

        if args.estimate_channel:
            if not header:
                print("  Skipping channel estimate: no .json sidecar with SRS config found.", file=sys.stderr)
                continue
            estimator = sce.get_estimator(args.estimate_channel, k_grid_offset=args.k_grid_offset)
            try:
                result = estimator.estimate(iq, header)
            except (NotImplementedError, ValueError) as exc:
                print(f"  Channel estimate failed: {exc}", file=sys.stderr)
                continue
            summarize_channel_estimate(result)
            if args.plot:
                plot_channel_estimate(npy_path, result, header)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
