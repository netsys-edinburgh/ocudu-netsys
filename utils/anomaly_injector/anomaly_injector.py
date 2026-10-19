#!/usr/bin/env python3
"""
anomaly-injector: sidecar that labels and drives the AURORA anomaly module of an OCUDU RAN process
(lib/aurora/anomaly).

The RAN process listens on 127.0.0.1:(47010 + ran_type) (gnb 47010, du 47011, cu 47012, cu_cp 47013, cu_up 47014).
In a pod the sidecar shares the network namespace, so it reaches that port on 127.0.0.1.

Label: the sidecar is the only source of the anomaly_label column. It sends "LABEL value=<label>" when the label
changes, and again every ANOMALY_LABEL_RESEND_S seconds (default 1) so that a restarted RAN process gets it back. The
RAN process keeps the last label and stamps it on every AURORA sample.

Anomalies: the RAN process injects MAC_CONTENTION, PDCP_CONTENTION, L1_THREAD_CONTENTION, QUEUE_TUNING and
MEMORY_LEAK itself (SET); anything else (network impairment, host stress, ...) is injected by another tool, and the
sidecar only labels it.

Commands:
  anomaly-injector                          daemon: plays $ANOMALY_SCHEDULE when set, otherwise waits for commands
  anomaly-injector run [--schedule F|JSON]  daemon playing a schedule
  anomaly-injector idle                     daemon without schedule (label and anomalies set through its control port)
  anomaly-injector label link_flap          sets the label: through the running daemon if there is one, else direct
  anomaly-injector send SET type=MAC_CONTENTION duration_ms=60000 load_min=10 load_max=80
  anomaly-injector status

Control port (daemon): 127.0.0.1:$ANOMALY_INJECTOR_PORT (default 47020), same line protocol as the RAN process.
"LABEL value=..." updates the daemon's label (and so the RAN process); every other line is forwarded to the RAN process
and its reply returned. Other containers of the pod (e.g. a network impairment tool) label their anomalies there:
  echo "LABEL value=link_flap" | nc 127.0.0.1 47020 ... echo "LABEL value=normal" | nc 127.0.0.1 47020

Schedule (JSON):
  {
    "repeat": true,                    # loop over the steps forever (default false)
    "start_delay_ms": 0,               # wait before the first step, after the RAN process answers PING
    "steps": [                         # fixed steps, played in order
      {"type": "NONE", "duration_ms": 60000},
      {"type": "MAC_CONTENTION", "duration_ms": 120000, "load_min": 10, "load_max": 80, "step_ms": 10000},
      {"type": "LABEL_ONLY", "label": "link_flap", "duration_ms": 30000}
    ],
    "random": {                        # instead of "steps": random RAN-injected anomalies separated by normal gaps
      "types": ["MAC_CONTENTION", "PDCP_CONTENTION"],
      "duration_ms": [30000, 120000],  # [min, max] of every range
      "gap_ms": [30000, 90000],
      "load_min": [5, 30],
      "load_max": [40, 95],
      "step_ms": 5000,
      "count": 0,                      # number of anomalies, 0 = forever
      "seed": 1
    }
  }
Step "type": "NONE" is a normal period (label "normal"); "LABEL_ONLY" only sets "label" for the step (the anomaly comes
from elsewhere); any other type is sent as SET, labelled with "label" or by default the lower-case type, with every
other key passed as key=value (load, load_min, load_max, step_ms, slice_ms, prio_offset, cell, method, delay_us,
jitter_us, bytes_per_sec, target). The label is set once the RAN process confirmed the SET and back to "normal" after
the step's CLEAR, so labelled samples always lie inside the anomaly.

Environment: AURORA_ANOMALY_HOST (127.0.0.1), AURORA_ANOMALY_PORT (47010), AURORA_ANOMALY_TOKEN, ANOMALY_SCHEDULE
(path or inline JSON), ANOMALY_CONNECT_TIMEOUT_S (0 = wait forever for the RAN process), ANOMALY_LABEL_RESEND_S (1;
0 = only on change), ANOMALY_INJECTOR_PORT (47020; 0 = no control port).

Every command and reply is logged to stdout as one JSON line with the wall-clock time (ts_us), which matches the
timestamp_us of the AURORA samples. Periodic label re-sends are not logged.
"""

import argparse
import json
import os
import random
import re
import signal
import socket
import socketserver
import sys
import threading
import time

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 47010
DEFAULT_CONTROL_PORT = 47020
LABEL_RE = re.compile(r"^[a-z0-9_+-]{1,31}$")
PASS_THROUGH_KEYS = (
    "target",
    "load",
    "load_min",
    "load_max",
    "step_ms",
    "slice_ms",
    "prio_offset",
    "cell",
    "method",
    "delay_us",
    "jitter_us",
    "bytes_per_sec",
)

_log_lock = threading.Lock()


def log(event, **fields):
    rec = {"ts_us": int(time.time() * 1e6), "event": event}
    rec.update(fields)
    with _log_lock:
        print(json.dumps(rec, separators=(",", ":")), flush=True)


def check_label(label):
    if not LABEL_RE.match(label):
        raise ValueError(f"invalid label {label!r}: 1-31 characters of a-z 0-9 _ + -")
    return label


class Client:
    """One TCP connection per command to the RAN process' anomaly server."""

    def __init__(self, host, port, token):
        self.host = host
        self.port = port
        self.token = token

    def send(self, line, quiet=False):
        """Sends one command line, returns the reply line (without newline) or raises OSError."""
        if self.token and line.split()[0].upper() in ("SET", "CLEAR", "LABEL") and "token=" not in line:
            line = f"{line} token={self.token}"
        reply = request(self.host, self.port, line)
        if not quiet:
            shown = line.replace(f"token={self.token}", "token=***") if self.token else line
            log("command", cmd=shown, reply=reply, ok=reply.startswith("OK"))
        return reply

    def wait_ready(self, timeout_s, stop):
        start = time.monotonic()
        announced = False
        while not stop.is_set():
            try:
                reply = self.send("PING", quiet=True)
                if reply.startswith("OK"):
                    log("ready", host=self.host, port=self.port, reply=reply)
                    return True
            except OSError as e:
                if not announced:
                    log("waiting", host=self.host, port=self.port, error=str(e))
                    announced = True
            if timeout_s and time.monotonic() - start > timeout_s:
                return False
            stop.wait(1)
        return False


def request(host, port, line):
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(line.encode() + b"\n")
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = s.recv(1024)
            if not chunk:
                break
            reply += chunk
    return reply.decode(errors="replace").strip()


class LabelKeeper:
    """Holds the current label: sends it when it changes and re-sends it every resend_s seconds."""

    def __init__(self, client, resend_s, stop):
        self.client = client
        self.resend_s = resend_s
        self.stop = stop
        self.label = "normal"
        self.lock = threading.Lock()
        self.changed = threading.Event()

    def set(self, label, source="schedule"):
        """Sets the label; returns the RAN process reply (sent synchronously when the label changes)."""
        check_label(label)
        with self.lock:
            changed = label != self.label
            self.label = label
        if not changed:
            return f"OK label={label} changed=0 (unchanged, not sent)"
        log("label", label=label, source=source)
        try:
            return self.client.send(f"LABEL value={label}")
        except OSError as e:
            log("error", cmd=f"LABEL {label}", error=str(e))
            return f"ERR code=UNREACHABLE message={e}"

    def run(self):
        while not self.stop.wait(self.resend_s if self.resend_s > 0 else 3600):
            if self.resend_s <= 0:
                continue
            with self.lock:
                label = self.label
            try:
                self.client.send(f"LABEL value={label}", quiet=True)
            except OSError:
                pass  # RAN process down; the next re-send after it is back restores the label


def start_control_server(port, client, keeper):
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            line = self.rfile.readline(1024).decode(errors="replace").strip()
            if not line:
                return
            words = line.split()
            if words[0].upper() == "LABEL":
                args = dict(w.split("=", 1) for w in words[1:] if "=" in w)
                try:
                    reply = keeper.set(args.get("value", ""), source="control")
                except ValueError as e:
                    reply = f"ERR code=BAD_ARG message={str(e).replace(' ', '_')}"
            else:
                try:
                    reply = client.send(line)
                except OSError as e:
                    reply = f"ERR code=UNREACHABLE message={e}"
            self.wfile.write(reply.encode() + b"\n")

    class Server(socketserver.ThreadingTCPServer):
        allow_reuse_address = True
        daemon_threads = True

    server = Server(("127.0.0.1", port), Handler)
    threading.Thread(target=server.serve_forever, name="control", daemon=True).start()
    log("control", port=port)
    return server


def step_command(step):
    parts = [f"SET type={step['type']}", f"duration_ms={int(step['duration_ms'])}"]
    for key in PASS_THROUGH_KEYS:
        if key in step:
            parts.append(f"{key}={step[key]}")
    unknown = set(step) - set(PASS_THROUGH_KEYS) - {"type", "duration_ms", "label"}
    if unknown:
        raise ValueError(f"unknown step keys: {sorted(unknown)}")
    return " ".join(parts)


def play_step(client, keeper, step, stop):
    kind = str(step.get("type", "")).upper()
    duration_ms = int(step.get("duration_ms", 0))
    if duration_ms <= 0:
        raise ValueError(f"step without a positive duration_ms: {step}")
    deadline = time.monotonic() + duration_ms / 1000.0
    if kind == "NONE":
        keeper.set("normal")
        stop.wait(max(0.0, deadline - time.monotonic()))
        return
    if kind == "LABEL_ONLY":
        keeper.set(check_label(step["label"]))
        stop.wait(max(0.0, deadline - time.monotonic()))
        keeper.set("normal")
        return
    label = check_label(step.get("label", kind.lower()))
    try:
        ok = client.send(step_command(dict(step, type=kind))).startswith("OK")
    except OSError as e:
        log("error", cmd=kind, error=str(e))
        ok = False
    if ok:
        keeper.set(label)
    stop.wait(max(0.0, deadline - time.monotonic()))
    # The RAN process ends the anomaly by itself; CLEAR only makes sure, e.g. when a reply was lost.
    try:
        client.send(f"CLEAR type={kind}")
    except OSError as e:
        log("error", cmd=f"CLEAR {kind}", error=str(e))
    keeper.set("normal")


def pick(rng, value):
    if isinstance(value, (list, tuple)):
        lo, hi = int(value[0]), int(value[-1])
        return rng.randint(min(lo, hi), max(lo, hi))
    return int(value)


def random_steps(cfg):
    rng = random.Random(cfg.get("seed"))
    types = cfg.get("types", ["MAC_CONTENTION", "PDCP_CONTENTION"])
    count = int(cfg.get("count", 0))
    n = 0
    while count == 0 or n < count:
        yield {"type": "NONE", "duration_ms": pick(rng, cfg.get("gap_ms", [30000, 90000]))}
        kind = rng.choice(types)
        step = {"type": kind, "duration_ms": pick(rng, cfg.get("duration_ms", [30000, 120000]))}
        if kind in ("MAC_CONTENTION", "PDCP_CONTENTION"):
            lo = pick(rng, cfg.get("load_min", [5, 30]))
            hi = max(lo, pick(rng, cfg.get("load_max", [40, 95])))
            step.update(load_min=lo, load_max=hi, step_ms=pick(rng, cfg.get("step_ms", 5000)))
            for key in ("slice_ms", "prio_offset", "cell"):
                if key in cfg:
                    step[key] = pick(rng, cfg[key])
        yield step
        n += 1


def load_schedule(value):
    text = value
    if not value.lstrip().startswith("{"):
        with open(value, encoding="utf-8") as f:
            text = f.read()
    return json.loads(text)


def run_daemon(client, schedule, stop):
    keeper = LabelKeeper(client, float(os.environ.get("ANOMALY_LABEL_RESEND_S", "1")), stop)
    control_port = int(os.environ.get("ANOMALY_INJECTOR_PORT", DEFAULT_CONTROL_PORT))
    server = start_control_server(control_port, client, keeper) if control_port else None

    if not client.wait_ready(float(os.environ.get("ANOMALY_CONNECT_TIMEOUT_S", "0")), stop):
        if not stop.is_set():
            log("error", error="RAN process not reachable")
        return 0 if stop.is_set() else 1
    # Start from a known label (a sidecar restart must not leave a stale one behind).
    keeper.label = None
    keeper.set("normal", source="start")
    threading.Thread(target=keeper.run, name="label-resend", daemon=True).start()

    try:
        if schedule is None:
            log("idle", host=client.host, port=client.port)
            stop.wait()
        else:
            log("schedule", schedule=schedule)
            if schedule.get("start_delay_ms"):
                stop.wait(int(schedule["start_delay_ms"]) / 1000.0)
            while not stop.is_set():
                steps = random_steps(schedule["random"]) if "random" in schedule else iter(schedule.get("steps", []))
                for step in steps:
                    if stop.is_set():
                        break
                    play_step(client, keeper, step, stop)
                if not schedule.get("repeat", False) or "random" in schedule:
                    break
            log("done")
            if not stop.is_set():
                stop.wait()  # keep holding the label (and the control port) until the pod stops
    finally:
        if schedule is not None:
            try:
                client.send("CLEAR type=ALL")
            except OSError:
                pass
        keeper.set("normal", source="stop")
        if server:
            server.shutdown()
    return 0


def main():
    parser = argparse.ArgumentParser(description="AURORA anomaly injector sidecar")
    parser.add_argument("--host", default=os.environ.get("AURORA_ANOMALY_HOST", DEFAULT_HOST))
    parser.add_argument("--port", type=int, default=int(os.environ.get("AURORA_ANOMALY_PORT", DEFAULT_PORT)))
    sub = parser.add_subparsers(dest="cmd")
    p_run = sub.add_parser("run", help="daemon playing a schedule")
    p_run.add_argument("--schedule", help="schedule file or inline JSON (default: $ANOMALY_SCHEDULE)")
    sub.add_parser("idle", help="daemon without schedule")
    p_label = sub.add_parser("label", help="set the anomaly label")
    p_label.add_argument("value")
    p_send = sub.add_parser("send", help="send one command line to the RAN process")
    p_send.add_argument("line", nargs="+")
    sub.add_parser("status", help="print the anomaly module status")
    args = parser.parse_args()

    if args.cmd is None:
        args.cmd = "run" if os.environ.get("ANOMALY_SCHEDULE") else "idle"
        args.schedule = None

    client = Client(args.host, args.port, os.environ.get("AURORA_ANOMALY_TOKEN", ""))

    if args.cmd == "label":
        line = f"LABEL value={check_label(args.value)}"
        control_port = int(os.environ.get("ANOMALY_INJECTOR_PORT", DEFAULT_CONTROL_PORT))
        try:
            # Through the running daemon, which would otherwise re-send its own label within a second.
            reply = request("127.0.0.1", control_port, line) if control_port else None
        except OSError:
            reply = None
        if reply is None:
            reply = client.send(line, quiet=True)
        log("command", cmd=line, reply=reply, ok=reply.startswith("OK"))
        return 0 if reply.startswith("OK") else 1
    if args.cmd == "send":
        return 0 if client.send(" ".join(args.line)).startswith("OK") else 1
    if args.cmd == "status":
        return 0 if client.send("STATUS").startswith("OK") else 1

    stop = threading.Event()
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    schedule = None
    if args.cmd == "run":
        value = args.schedule or os.environ.get("ANOMALY_SCHEDULE", "")
        if not value:
            raise SystemExit("no schedule: pass --schedule or set ANOMALY_SCHEDULE")
        schedule = load_schedule(value)
    return run_daemon(client, schedule, stop)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ValueError as e:
        log("error", error=str(e))
        sys.exit(2)
