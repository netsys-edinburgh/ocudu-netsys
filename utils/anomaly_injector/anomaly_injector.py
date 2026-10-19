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
  anomaly-injector inject --label mac_contention MAC_CONTENTION duration_ms=60000 load=40   (through the daemon)
  anomaly-injector lease --ttl 60                (daemon: renew the anomaly-ctl lease)
  anomaly-injector daemon-status                 (daemon: label, lease and anomaly time left)

Control port (daemon): 127.0.0.1:$ANOMALY_INJECTOR_PORT (default 47020), same line protocol as the RAN process.
"LABEL value=..." updates the daemon's label (and so the RAN process); every other line is forwarded to the RAN process
and its reply returned. Other containers of the pod (e.g. a network impairment tool) label their anomalies there:
  echo "LABEL value=link_flap" | nc 127.0.0.1 47020 ... echo "LABEL value=normal" | nc 127.0.0.1 47020
Also: "INJECT label=<l> type=<T> <SET params>" (SET, then label on OK), "LABEL value=<l> duration_ms=<ms>" (the
watchdog learns when an external anomaly ends), "LEASE ttl_s=<s>", "DAEMON".

Watchdog (anomaly-ctl, utils/anomaly_injector/anomaly_ctl.py): armed by the first LEASE. When the lease runs out while
the label is not "normal" (controller gone), the current anomaly is let to end, then the label becomes "recovery" and
the sidecar restarts its own pod through the Kubernetes API (patch of ANOMALY_RESTART_DEPLOYMENT, service account
token under /var/run/secrets/kubernetes.io/serviceaccount); the new pod stays "normal".

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
0 = only on change), ANOMALY_INJECTOR_PORT (47020; 0 = no control port), ANOMALY_RESTART_DEPLOYMENT (watchdog).

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


def _iface_mac(iface):
    with open(f"/sys/class/net/{iface}/address", encoding="utf-8") as f:
        return bytes(int(b, 16) for b in f.read().strip().split(":"))


def _parse_mac(text):
    parts = text.split(":")
    if len(parts) != 6:
        raise ValueError(f"invalid MAC {text!r}")
    return bytes(int(b, 16) for b in parts)


class FronthaulSender:
    """Floods raw Ethernet frames out a kernel fronthaul interface (a VLAN-103 iavf VF, e.g. fhinj0) toward the RU MAC,
    to contend with the real O-RAN 7.2 U-plane on the shared NIC Tx path / switch egress. The bitrate ramps linearly
    from rate_min_mbps to rate_max_mbps over the duration, one step every step_ms (like the stress-ng load ramp). Needs
    CAP_NET_RAW. Only the AURORA / control threads start and stop it; one sender at a time."""

    # Ethernet wire overhead per frame: VLAN tag 4 + FCS 4 + preamble 8 + inter-frame gap 12.
    WIRE_OVERHEAD = 28

    def __init__(self, stop):
        self.stop = stop
        self.lock = threading.Lock()
        self.thread = None
        self.run_flag = False
        self.cur_mbps = 0.0
        self.sent_bytes = 0

    def active(self):
        return self.thread is not None and self.thread.is_alive()

    def start(self, params):
        """params: iface, dst_mac, rate_min_mbps, rate_max_mbps, step_ms, duration_ms, frame_size, ethertype.
        Returns a one-line OK/ERR string."""
        self.stop_sender()
        iface = params.get("iface") or os.environ.get("AURORA_FH_IFACE", "")
        if not iface:
            return "ERR code=BAD_ARG message=no_fronthaul_iface_(AURORA_FH_IFACE)"
        dst = params.get("dst_mac") or os.environ.get("AURORA_FH_DST_MAC", "")
        if not dst:
            return "ERR code=BAD_ARG message=no_fronthaul_dst_mac_(AURORA_FH_DST_MAC)"
        frame_size = int(params.get("frame_size", os.environ.get("AURORA_FH_FRAME_SIZE", "1500")))
        frame_size = max(64, min(frame_size, 9000))
        ethertype = int(str(params.get("ethertype", os.environ.get("AURORA_FH_ETHERTYPE", "0x88b5"))), 0)
        rate_min = float(params.get("rate_min_mbps", params.get("rate_mbps", 100)))
        rate_max = float(params.get("rate_max_mbps", rate_min))
        if rate_min <= 0 or rate_max < rate_min:
            return "ERR code=BAD_ARG message=rate_min_mbps_<=0_or_>rate_max_mbps"
        step_ms = max(100, int(params.get("step_ms", 1000)))
        duration_ms = int(params.get("duration_ms", 10000))
        if duration_ms <= 0:
            return "ERR code=BAD_ARG message=duration_ms_<=0"
        try:
            src = _iface_mac(iface)
            frame = _parse_mac(dst) + src + ethertype.to_bytes(2, "big")
            frame += bytes(frame_size - len(frame)) if frame_size > len(frame) else b""
            sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, 0)
            sock.bind((iface, 0))
        except (OSError, ValueError, FileNotFoundError) as e:
            return f"ERR code=FRONTHAUL_FAILED message={str(e).replace(' ', '_')}"

        self.run_flag = True
        self.sent_bytes = 0
        self.thread = threading.Thread(
            target=self._run,
            args=(sock, frame, rate_min, rate_max, step_ms, duration_ms),
            name="fronthaul", daemon=True)
        self.thread.start()
        steps = max(1, (duration_ms + step_ms - 1) // step_ms)
        return (f"OK applied=1 fronthaul iface={iface} dst={dst} rate_min_mbps={rate_min:g} "
                f"rate_max_mbps={rate_max:g} step_ms={step_ms} steps={steps} frame_size={frame_size} "
                f"ethertype=0x{ethertype:04x} duration_ms={duration_ms}")

    def _run(self, sock, frame, rate_min, rate_max, step_ms, duration_ms):
        wire_bits = (len(frame) + self.WIRE_OVERHEAD) * 8
        nof_steps = max(1, (duration_ms + step_ms - 1) // step_ms)
        start = time.monotonic()
        end = start + duration_ms / 1000.0
        try:
            while self.run_flag and not self.stop.is_set() and time.monotonic() < end:
                step = min(nof_steps - 1, int((time.monotonic() - start) * 1000 // step_ms))
                mbps = rate_max if nof_steps == 1 else rate_min + (rate_max - rate_min) * step / (nof_steps - 1)
                self.cur_mbps = mbps
                rate_bps = mbps * 1e6
                step_end = min(end, start + (step + 1) * step_ms / 1000.0)
                base = time.monotonic()
                sent_bits = 0
                while self.run_flag and not self.stop.is_set() and time.monotonic() < step_end:
                    target_bits = (time.monotonic() - base) * rate_bps
                    while sent_bits < target_bits and time.monotonic() < step_end:
                        try:
                            sock.send(frame)
                        except OSError:
                            break
                        sent_bits += wire_bits
                        self.sent_bytes += len(frame)
                    time.sleep(0.0002)
        finally:
            self.cur_mbps = 0.0
            try:
                sock.close()
            except OSError:
                pass

    def stop_sender(self):
        self.run_flag = False
        with self.lock:
            t = self.thread
        if t and t.is_alive():
            t.join(timeout=3)
        self.thread = None
        self.cur_mbps = 0.0


SA_DIR = "/var/run/secrets/kubernetes.io/serviceaccount"


def restart_own_deployment():
    """Restarts this pod's Deployment like `kubectl rollout restart` (patches the pod template annotation), through
    the Kubernetes API with the pod's service account. Needs ANOMALY_RESTART_DEPLOYMENT and RBAC "patch" on it."""
    # Imported here: every `kubectl exec ... anomaly-injector` pays for module imports, and this is rarely needed.
    import ssl
    import urllib.request
    from datetime import datetime, timezone

    name = os.environ.get("ANOMALY_RESTART_DEPLOYMENT", "")
    if not name:
        raise RuntimeError("ANOMALY_RESTART_DEPLOYMENT not set")
    sa_dir = os.environ.get("ANOMALY_SA_DIR", SA_DIR)
    with open(os.path.join(sa_dir, "token"), encoding="utf-8") as f:
        token = f.read().strip()
    with open(os.path.join(sa_dir, "namespace"), encoding="utf-8") as f:
        namespace = f.read().strip()
    host = os.environ["KUBERNETES_SERVICE_HOST"]
    port = os.environ.get("KUBERNETES_SERVICE_PORT", "443")
    if ":" in host:
        host = f"[{host}]"
    body = {
        "spec": {
            "template": {
                "metadata": {
                    "annotations": {
                        "kubectl.kubernetes.io/restartedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
                        "aurora.ocudu/restarted-by": "anomaly-injector-watchdog",
                    }
                }
            }
        }
    }
    req = urllib.request.Request(
        f"https://{host}:{port}/apis/apps/v1/namespaces/{namespace}/deployments/{name}",
        data=json.dumps(body).encode(),
        method="PATCH",
        headers={"Authorization": f"Bearer {token}", "Content-Type": "application/strategic-merge-patch+json"},
    )
    ctx = ssl.create_default_context(cafile=os.path.join(sa_dir, "ca.crt"))
    with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
        return resp.status


class Watchdog:
    """Lease from the anomaly-ctl controller. When it runs out (controller gone) while an anomaly is labelled, waits for
    the anomaly to end, labels "recovery" and restarts this pod, which then stays "normal". Not armed before the first
    LEASE, so a sidecar used without controller never restarts its pod."""

    RESTART_RETRY_S = 10

    def __init__(self, client, keeper, stop, fronthaul):
        self.client = client
        self.keeper = keeper
        self.stop = stop
        self.fronthaul = fronthaul
        self.lock = threading.Lock()
        self.lease_until = None   # monotonic
        self.anomaly_until = None  # monotonic, None = no end known (ends now)
        self.last_restart = None

    def lease(self, ttl_s):
        with self.lock:
            first = self.lease_until is None
            self.lease_until = time.monotonic() + ttl_s
        if first:
            log("lease", ttl_s=ttl_s)

    def anomaly(self, duration_s):
        with self.lock:
            self.anomaly_until = time.monotonic() + duration_s if duration_s else None

    def state(self):
        now = time.monotonic()
        with self.lock:
            lease = None if self.lease_until is None else round(self.lease_until - now, 1)
            anomaly = None if self.anomaly_until is None else round(max(0.0, self.anomaly_until - now), 1)
        return lease, anomaly

    def run(self):
        while not self.stop.wait(1):
            now = time.monotonic()
            with self.lock:
                lost = self.lease_until is not None and now >= self.lease_until
                anomaly_until = self.anomaly_until
            if not lost:
                continue
            label = self.keeper.label
            if label == "normal":
                continue  # nothing to recover from
            if label != "recovery":
                if anomaly_until is not None and now < anomaly_until:
                    continue  # let the current anomaly run to its end
                log("lease_lost", label=label)
                try:
                    self.client.send("CLEAR type=ALL")
                except OSError as e:
                    log("error", cmd="CLEAR ALL", error=str(e))
                self.fronthaul.stop_sender()
                self.keeper.set("recovery", source="watchdog")
            if self.last_restart is None or now - self.last_restart >= self.RESTART_RETRY_S:
                self.last_restart = now
                try:
                    status = restart_own_deployment()
                    log("restart", by="watchdog", http_status=status)
                except Exception as e:  # noqa: BLE001 - keep retrying whatever failed
                    log("error", cmd="restart", error=str(e))


def start_control_server(port, client, keeper, watchdog, fronthaul):
    def parse_args(words):
        return dict(w.split("=", 1) for w in words if "=" in w)

    def default_multi_label(types):
        order = ["mac_contention", "pdcp_contention", "l1_thread_contention", "queue_tuning", "memory_leak",
                 "fronthaul"]
        names = sorted({t.lower() for t in types}, key=lambda t: order.index(t) if t in order else len(order))
        return "+".join(names)

    def handle_multi(payload):
        try:
            spec = json.loads(payload)
            anomalies = spec["anomalies"]
            duration_ms = int(spec.get("duration_ms", 10000))
        except (ValueError, KeyError, TypeError) as e:
            return f"ERR code=BAD_ARG message=bad_MULTI_json_{str(e).replace(' ', '_')}"
        label = spec.get("label") or default_multi_label([a.get("type", "") for a in anomalies])
        try:
            check_label(label)
        except ValueError as e:
            return f"ERR code=BAD_ARG message={str(e).replace(' ', '_')}"

        applied_ran = False
        applied_fh = False
        replies = []
        for a in anomalies:
            kind = str(a.get("type", "")).upper()
            a = dict(a, duration_ms=a.get("duration_ms", duration_ms))
            if kind == "FRONTHAUL":
                r = fronthaul.start(a)
                applied_fh = applied_fh or r.startswith("OK")
            else:
                params = " ".join(f"{k}={v}" for k, v in a.items() if k != "type")
                r = client.send(f"SET type={kind} {params}")
                applied_ran = applied_ran or r.startswith("OK")
            replies.append(f"{kind}:{'OK' if r.startswith('OK') else r}")
            if not r.startswith("OK"):
                # Roll back what was applied, so a partial MULTI never runs mislabelled.
                fronthaul.stop_sender()
                if applied_ran:
                    try:
                        client.send("CLEAR type=ALL")
                    except OSError:
                        pass
                return f"ERR code=MULTI_FAILED message={r}"
        watchdog.anomaly(duration_ms / 1000.0)
        keeper.set(label, source="control")
        return f"OK applied={len(anomalies)} label={label} [{'; '.join(replies)}]"

    def handle_line(line):
        words = line.split()
        cmd = words[0].upper()
        if cmd == "LABEL":
            args = parse_args(words[1:])
            reply = keeper.set(args.get("value", ""), source="control")
            if reply.startswith("OK"):
                # duration_ms: when an anomaly injected elsewhere ends (used by the watchdog only).
                watchdog.anomaly(int(args.get("duration_ms", "0")) / 1000.0 if args.get("value") != "normal" else 0)
            return reply
        if cmd == "INJECT":
            # INJECT label=<label> type=<TYPE> <params>: for FRONTHAUL the sidecar floods raw frames itself; for the
            # RAN-process anomalies it forwards SET. In both cases the label is set once it is applied.
            args = parse_args(words[1:])
            label = check_label(args.pop("label", str(args.get("type", "")).lower()))
            if args.get("type", "").upper() == "FRONTHAUL":
                reply = fronthaul.start(args)
            else:
                rest = " ".join(w for w in words[1:] if not w.startswith("label="))
                reply = client.send(f"SET {rest}")
            if reply.startswith("OK"):
                watchdog.anomaly(int(args.get("duration_ms", "10000")) / 1000.0)
                keeper.set(label, source="control")
            return reply
        if cmd == "MULTI":
            # MULTI {json}: several anomalies at once, applied all-or-nothing, one label. json has "label",
            # "duration_ms", "anomalies":[{type, ...}]. FRONTHAUL runs in the sidecar, the rest are SET to the RAN.
            return handle_multi(line[len("MULTI"):].strip())
        if cmd == "CLEAR":
            # Stop the local fronthaul flood too, then forward to the RAN process.
            if parse_args(words[1:]).get("type", "ALL").upper() in ("ALL", "FRONTHAUL"):
                fronthaul.stop_sender()
            return client.send(line)
        if cmd == "LEASE":
            ttl = float(parse_args(words[1:]).get("ttl_s", "60"))
            watchdog.lease(ttl)
            return f"OK lease ttl_s={ttl:g}"
        if cmd == "DAEMON":
            lease, anomaly = watchdog.state()
            fh = f"{fronthaul.cur_mbps:g}" if fronthaul.active() else "-"
            return (f"OK label={keeper.label} lease_s={'-' if lease is None else lease} "
                    f"anomaly_s={'-' if anomaly is None else anomaly} fronthaul_mbps={fh}")
        return client.send(line)

    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            line = self.rfile.readline(1024).decode(errors="replace").strip()
            if not line:
                return
            try:
                reply = handle_line(line)
            except ValueError as e:
                reply = f"ERR code=BAD_ARG message={str(e).replace(' ', '_')}"
            except OSError as e:
                reply = f"ERR code=UNREACHABLE message={str(e).replace(' ', '_')}"
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
    fronthaul = FronthaulSender(stop)
    watchdog = Watchdog(client, keeper, stop, fronthaul)
    control_port = int(os.environ.get("ANOMALY_INJECTOR_PORT", DEFAULT_CONTROL_PORT))
    server = start_control_server(control_port, client, keeper, watchdog, fronthaul) if control_port else None

    if not client.wait_ready(float(os.environ.get("ANOMALY_CONNECT_TIMEOUT_S", "0")), stop):
        if not stop.is_set():
            log("error", error="RAN process not reachable")
        return 0 if stop.is_set() else 1
    # Start from a known label (a sidecar restart must not leave a stale one behind).
    keeper.label = None
    keeper.set("normal", source="start")
    threading.Thread(target=keeper.run, name="label-resend", daemon=True).start()
    threading.Thread(target=watchdog.run, name="watchdog", daemon=True).start()

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
        # The label and the anomaly are left as they are: SIGTERM means the pod (RAN process included) is stopping, and
        # the samples it still publishes meanwhile belong to the current label (e.g. "recovery", or an anomaly that
        # runs until the pod is gone). A new sidecar always starts from "normal".
        log("stop", label=keeper.label)
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
    p_label.add_argument("--duration-ms", type=int, default=0,
                         help="when the anomaly injected elsewhere ends (lets the watchdog recover after it)")
    p_inject = sub.add_parser("inject", help="daemon: SET an anomaly and label it once the RAN process confirmed")
    p_inject.add_argument("--label", help="label (default: lower-case type)")
    p_inject.add_argument("type")
    p_inject.add_argument("params", nargs="*", help="SET parameters key=value (duration_ms=..., load_min=..., ...)")
    p_lease = sub.add_parser("lease", help="daemon: renew the controller lease")
    p_lease.add_argument("--ttl", type=float, default=60.0)
    p_multi = sub.add_parser("multi", help="daemon: inject several anomalies at once (JSON)")
    p_multi.add_argument("json", help='{"label":..,"duration_ms":..,"anomalies":[{"type":..,..},..]}')
    sub.add_parser("daemon-status", help="daemon: label, lease and anomaly time left")
    p_send = sub.add_parser("send", help="send one command line to the RAN process")
    p_send.add_argument("line", nargs="+")
    sub.add_parser("status", help="print the anomaly module status")
    args = parser.parse_args()

    if args.cmd is None:
        args.cmd = "run" if os.environ.get("ANOMALY_SCHEDULE") else "idle"
        args.schedule = None

    client = Client(args.host, args.port, os.environ.get("AURORA_ANOMALY_TOKEN", ""))

    control_port = int(os.environ.get("ANOMALY_INJECTOR_PORT", DEFAULT_CONTROL_PORT))
    if args.cmd in ("inject", "multi", "lease", "daemon-status"):
        if args.cmd == "inject":
            label = check_label(args.label or args.type.lower())
            line = " ".join([f"INJECT label={label} type={args.type.upper()}"] + args.params)
        elif args.cmd == "multi":
            json.loads(args.json)  # validate before sending
            line = "MULTI " + args.json
        elif args.cmd == "lease":
            line = f"LEASE ttl_s={args.ttl:g}"
        else:
            line = "DAEMON"
        reply = request("127.0.0.1", control_port, line)
        log("command", cmd=line, reply=reply, ok=reply.startswith("OK"))
        return 0 if reply.startswith("OK") else 1
    if args.cmd == "label":
        line = f"LABEL value={check_label(args.value)}"
        if args.duration_ms:
            line += f" duration_ms={args.duration_ms}"
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
