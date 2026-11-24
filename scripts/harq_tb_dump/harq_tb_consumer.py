#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Consumer of the per-TB HARQ statistics streamed by the gNB (`harq_tb_dump` option).

It connects a ZeroMQ PULL socket to the gNB and appends every received record to
`<out-dir>/harq_tb_<start time>.bin`. A .json file with the same name summarizes the run: number of records and
batches, lost batches (gaps in the batch sequence number), records dropped by the gNB, and gNB restarts. It is
rewritten periodically and when the consumer stops, on Ctrl+C or SIGTERM.

Connect only one consumer at a time: a ZeroMQ PUSH socket distributes the batches among all the connected consumers
instead of copying them, so each consumer would only get a part of the data.

Usage:
    pip install pyzmq
    python3 harq_tb_consumer.py tcp://127.0.0.1:5555 --out-dir /data/harq_dump

    # Record for 10 minutes and stop:
    python3 harq_tb_consumer.py tcp://127.0.0.1:5555 --out-dir /data/harq_dump --duration 600

    # Run in the background, e.g. inside a Kubernetes sidecar, and stop it later:
    nohup python3 harq_tb_consumer.py tcp://127.0.0.1:5555 --out-dir /data/harq_dump \\
        --pid-file /data/harq_dump/consumer.pid > /data/harq_dump/consumer.log 2>&1 &
    kill -TERM "$(cat /data/harq_dump/consumer.pid)"
"""

import argparse
import json
import os
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

import zmq

import harq_tb_format as htf

#: Maximum time to wait for a message before checking the stop conditions, in milliseconds.
POLL_TIMEOUT_MS = 500


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


class RunSummary:
    """Keeps the statistics of a run, which are saved to the .json file."""

    def __init__(self, address: str, bin_path: Path):
        self.info = {
            "format_version": htf.VERSION,
            "record_size": htf.RECORD_SIZE,
            "record_fields": list(htf.RECORD_FIELDS),
            "address": address,
            "bin_file": bin_path.name,
            "start_time": now_iso(),
            "end_time": None,
            "nof_records": 0,
            "nof_batches": 0,
            "nof_lost_batches": 0,
            "nof_malformed_messages": 0,
            # One entry per gNB process seen. A new entry starts when the batch sequence number goes back, i.e., when
            # the gNB restarts.
            "gnb_sessions": [],
        }

    def on_batch(self, header: dict) -> int:
        """Updates the statistics with a valid batch. Returns the number of batches lost before it."""
        seq = header["seq"]
        dropped = header["nof_dropped_total"]
        sessions = self.info["gnb_sessions"]
        lost = 0
        if not sessions or seq <= sessions[-1]["last_seq"]:
            # The first batch of the run, or of a restarted gNB. Its nof_dropped_total also counts the records dropped
            # before this consumer connected.
            sessions.append(
                {
                    "start_time": now_iso(),
                    "first_seq": seq,
                    "last_seq": seq,
                    "nof_lost_batches": 0,
                    "first_nof_dropped_total": dropped,
                    "last_nof_dropped_total": dropped,
                }
            )
        else:
            session = sessions[-1]
            lost = seq - session["last_seq"] - 1
            session["nof_lost_batches"] += lost
            session["last_seq"] = seq
            session["last_nof_dropped_total"] = dropped
            self.info["nof_lost_batches"] += lost
        self.info["nof_batches"] += 1
        self.info["nof_records"] += header["nof_records"]
        return lost

    def nof_dropped_during_run(self) -> int:
        """Records dropped by the gNB while this consumer was running, over all gNB sessions.

        Only the records that the first gNB session had dropped before this consumer connected are excluded. A
        restarted gNB starts after the consumer, so all its drops happened during the run.
        """
        sessions = self.info["gnb_sessions"]
        if not sessions:
            return 0
        first = sessions[0]
        return (first["last_nof_dropped_total"] - first["first_nof_dropped_total"]) + sum(
            s["last_nof_dropped_total"] for s in sessions[1:]
        )

    def save(self, path: Path) -> None:
        """Writes the .json file atomically, so that it is never left half-written."""
        self.info["nof_dropped_during_run"] = self.nof_dropped_during_run()
        tmp_path = path.with_name(path.name + ".tmp")
        with open(tmp_path, "w") as f:
            json.dump(self.info, f, indent=2)
        os.replace(tmp_path, path)


def make_base_path(out_dir: Path) -> Path:
    """Returns a base file name, without extension, that is not used yet."""
    base = out_dir / f"harq_tb_{datetime.now().strftime('%Y%m%d_%H%M%S')}"
    candidate = base
    index = 1
    while candidate.with_name(candidate.name + ".bin").exists():
        candidate = base.with_name(f"{base.name}_{index}")
        index += 1
    return candidate


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("address", help="ZeroMQ address of the gNB, e.g. tcp://127.0.0.1:5555")
    parser.add_argument("--out-dir", required=True, type=Path, help="Directory where the .bin and .json files go")
    parser.add_argument("--duration", type=float, default=None, help="Stop after this many seconds")
    parser.add_argument("--status-period", type=float, default=10.0, help="Seconds between status lines")
    parser.add_argument("--pid-file", type=Path, default=None, help="Write the process ID to this file")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)
    base_path = make_base_path(args.out_dir)
    bin_path = base_path.with_name(base_path.name + ".bin")
    json_path = base_path.with_name(base_path.name + ".json")
    summary = RunSummary(args.address, bin_path)

    if args.pid_file:
        args.pid_file.write_text(f"{os.getpid()}\n")

    # Stop cleanly on Ctrl+C (SIGINT) and on SIGTERM, which Kubernetes sends when a pod stops.
    stop_requested = False

    def on_stop_signal(signum, frame):
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGINT, on_stop_signal)
    signal.signal(signal.SIGTERM, on_stop_signal)

    context = zmq.Context()
    socket = context.socket(zmq.PULL)
    socket.connect(args.address)
    poller = zmq.Poller()
    poller.register(socket, zmq.POLLIN)
    print(f"Connecting to {args.address}, writing {bin_path}. Press Ctrl+C to stop.", file=sys.stderr)

    start = time.monotonic()
    deadline = start + args.duration if args.duration else None
    next_status = start + args.status_period
    warned_errors = set()

    try:
        with open(bin_path, "wb") as bin_file:
            while not stop_requested and (deadline is None or time.monotonic() < deadline):
                try:
                    events = dict(poller.poll(POLL_TIMEOUT_MS))
                except zmq.ZMQError as exc:
                    if exc.errno == zmq.EINTR:
                        continue
                    raise

                if socket in events:
                    parts = socket.recv_multipart(zmq.NOBLOCK)
                    try:
                        if len(parts) != 2:
                            raise htf.FormatError(f"message has {len(parts)} parts instead of 2")
                        header = htf.parse_header(parts[0])
                        payload = parts[1]
                        if len(payload) != header["nof_records"] * htf.RECORD_SIZE:
                            raise htf.FormatError(
                                f"payload has {len(payload)} bytes for {header['nof_records']} records"
                            )
                    except htf.FormatError as exc:
                        summary.info["nof_malformed_messages"] += 1
                        if str(exc) not in warned_errors:
                            warned_errors.add(str(exc))
                            print(f"WARNING: skipping malformed message: {exc}", file=sys.stderr)
                        continue

                    # Flush every batch, so that the data is on disk even if the consumer is killed.
                    bin_file.write(payload)
                    bin_file.flush()
                    lost = summary.on_batch(header)
                    if lost:
                        print(f"WARNING: {lost} batch(es) lost before seq={header['seq']}", file=sys.stderr)

                now = time.monotonic()
                if now >= next_status:
                    next_status = now + args.status_period
                    info = summary.info
                    print(
                        f"[{now - start:7.0f}s] records={info['nof_records']} batches={info['nof_batches']} "
                        f"lost_batches={info['nof_lost_batches']} "
                        f"dropped_by_gnb={summary.nof_dropped_during_run()} "
                        f"gnb_sessions={len(info['gnb_sessions'])}",
                        file=sys.stderr,
                    )
                    summary.save(json_path)
    finally:
        summary.info["end_time"] = now_iso()
        summary.save(json_path)
        socket.close(linger=0)
        context.term()
        if args.pid_file:
            args.pid_file.unlink(missing_ok=True)

    info = summary.info
    print(
        f"Stopped. {info['nof_records']} records in {info['nof_batches']} batches written to {bin_path}. "
        f"Lost batches: {info['nof_lost_batches']}. "
        f"Records dropped by the gNB during the run: {summary.nof_dropped_during_run()}.",
        file=sys.stderr,
    )
    if info["gnb_sessions"] and info["gnb_sessions"][0]["first_nof_dropped_total"] > 0:
        print(
            f"Note: the gNB had already dropped {info['gnb_sessions'][0]['first_nof_dropped_total']} records "
            "before this consumer connected.",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
