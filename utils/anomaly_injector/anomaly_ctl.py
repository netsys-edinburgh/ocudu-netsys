#!/usr/bin/env python3
"""
anomaly-ctl: runs AURORA anomaly campaigns on an OCUDU RAN pod from the management node, through `kubectl`.

Every segment of a campaign (normal or anomaly) starts from a freshly restarted pod:

  1. `kubectl rollout restart` the RAN Deployment and wait until it is Ready and the RAN process answers (retried up
     to --restart-retries times, then the campaign is aborted);
  2. warm-up: the aurora-reader of the new pod drops the samples of its first AURORA_WARMUP_S seconds (read from the
     Deployment, so controller and reader always agree);
  3. --inject-lead seconds before the warm-up ends, the anomaly is injected and labelled through the anomaly-injector
     sidecar (`kubectl exec ... anomaly-injector inject|label`), so every recorded sample of the segment lies inside
     the anomaly;
  4. the segment's duration_s is recorded, then the next segment restarts the pod.

The anomaly is injected with --margin seconds more than the segment needs, so it still runs when the next restart
stops the pod. While the campaign runs, the controller renews a lease in the sidecar every 10 s (valid 60 s). If the
controller disappears, the sidecar lets the current anomaly end, labels "recovery" and restarts its pod, which then
stays "normal".

At the end (or on Ctrl-C) the pod is restarted once more and left normal.

Usage:
  anomaly-ctl -n <namespace> -d <deployment> run campaign.json [--log run.jsonl]
  anomaly-ctl -n <namespace> -d <deployment> status
  anomaly-ctl -n <namespace> -d <deployment> restart
  anomaly-ctl -n <namespace> -d <deployment> label link_flap
  anomaly-ctl -n <namespace> -d <deployment> send STATUS

Campaign (JSON; "namespace" / "deployment" may be given here instead of on the command line):
  {
    "namespace": "campus5g-dev-ocudu-mono-aurora-inf3",
    "deployment": "ocudu-mono-aurora-inf3-gnb",
    "repeat": 1,                                   # times to play the segments (0 = forever)
    "segments": [
      {"type": "NONE", "duration_s": 600},
      {"type": "MAC_CONTENTION", "duration_s": 600, "load_min": 10, "load_max": 80, "step_ms": 30000},
      {"type": "PDCP_CONTENTION", "duration_s": 600, "load": 40, "label": "pdcp_contention_40"},
      {"type": "LABEL_ONLY", "label": "link_flap", "duration_s": 300}
    ],
    "random": {                                    # instead of "segments": random anomalies with normal gaps
      "types": ["MAC_CONTENTION", "PDCP_CONTENTION"], "duration_s": [300, 900], "gap_s": [300, 600],
      "load_min": [5, 30], "load_max": [40, 95], "step_ms": 30000, "count": 10, "seed": 1
    }
  }
"NONE" records a normal segment. "LABEL_ONLY" only labels the segment; the anomaly itself must come from another tool
(started by you within the segment's recorded window, logged by this script). Any other type is injected by the RAN
process (MAC_CONTENTION, PDCP_CONTENTION, L1_THREAD_CONTENTION, QUEUE_TUNING, MEMORY_LEAK) with the remaining keys
as SET parameters, labelled with "label" (default: the lower-case type).

The log (--log, JSON lines) holds every step with wall-clock times; each "segment_recorded" line gives the label and
the recorded window [record_start_us, record_end_us], comparable with the AURORA timestamp_us.
"""

import argparse
import json
import random
import re
import signal
import subprocess
import sys
import threading
import time

LEASE_RENEW_S = 10
LEASE_TTL_S = 60
# Injected by the RAN process (SET). FRONTHAUL is injected by the sidecar itself; MULTI bundles several.
INJECTED_TYPES = ("MAC_CONTENTION", "PDCP_CONTENTION", "L1_THREAD_CONTENTION", "QUEUE_TUNING", "MEMORY_LEAK",
                  "FRONTHAUL")


class Abort(Exception):
    pass


class Logger:
    def __init__(self, path):
        self.lock = threading.Lock()
        self.f = open(path, "a", encoding="utf-8") if path else None

    def __call__(self, event, quiet=False, **fields):
        rec = {"ts_us": int(time.time() * 1e6), "event": event}
        rec.update(fields)
        line = json.dumps(rec, separators=(",", ":"))
        with self.lock:
            if self.f:
                self.f.write(line + "\n")
                self.f.flush()
            if not quiet:
                print(line, flush=True)


class Kube:
    def __init__(self, args, log):
        self.base = [args.kubectl]
        if args.context:
            self.base += ["--context", args.context]
        if args.kubeconfig:
            self.base += ["--kubeconfig", args.kubeconfig]
        self.base += ["-n", args.namespace]
        self.deployment = args.deployment
        self.container = args.container
        self.log = log
        self.latencies = []  # recent `kubectl exec` round trips (s)

    def run(self, argv, timeout=60):
        try:
            p = subprocess.run(self.base + argv, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return 124, "", f"kubectl {' '.join(argv[:2])} timed out after {timeout} s"
        return p.returncode, p.stdout, p.stderr.strip()

    def injector(self, argv, timeout=30):
        """Runs `anomaly-injector <argv>` in the sidecar; returns the reply line ("" when it could not run)."""
        t0 = time.monotonic()
        rc, out, err = self.run(
            ["exec", f"deploy/{self.deployment}", "-c", self.container, "--", "anomaly-injector"] + argv, timeout)
        if rc == 0:
            self.latencies = (self.latencies + [time.monotonic() - t0])[-10:]
        reply = ""
        for line in out.splitlines():
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("event") == "command":
                reply = rec.get("reply", "")
            elif rec.get("event") == "error" and not reply:
                reply = "ERR " + rec.get("error", "")
        if not reply and rc != 0:
            reply = f"ERR kubectl exit {rc}: {err.splitlines()[-1] if err else ''}"
        return reply

    def exec_latency(self):
        """Worst recent `kubectl exec` round trip (s)."""
        return max(self.latencies) if self.latencies else 1.0

    def warmup_s(self):
        rc, out, err = self.run(["get", "deployment", self.deployment, "-o", "json"])
        if rc != 0:
            raise Abort(f"cannot read deployment {self.deployment}: {err}")
        spec = json.loads(out)["spec"]["template"]["spec"]
        for c in spec.get("containers", []) + spec.get("initContainers", []):
            for env in c.get("env", []) or []:
                if env.get("name") == "AURORA_WARMUP_S":
                    return float(env.get("value", "0"))
        return 0.0


class Controller:
    def __init__(self, args, log):
        self.args = args
        self.log = log
        self.kube = Kube(args, log)
        self.stop = threading.Event()
        self.lease_thread = None

    # -- pod -----------------------------------------------------------------------------------------------------------
    def restart_once(self):
        """Restarts the pod; returns the RAN process start (wall clock, its STATUS started_us: when its AURORA samples
        begin, which is where aurora_reader's warm-up starts), or None."""
        rc, _, err = self.kube.run(["rollout", "restart", f"deployment/{self.kube.deployment}"])
        if rc != 0:
            self.log("restart_failed", step="rollout restart", error=err)
            return None
        rc, _, err = self.kube.run(
            ["rollout", "status", f"deployment/{self.kube.deployment}", f"--timeout={int(self.args.ready_timeout)}s"],
            timeout=self.args.ready_timeout + 30)
        if rc != 0:
            self.log("restart_failed", step="rollout status", error=err)
            return None
        deadline = time.monotonic() + self.args.ready_timeout
        while not self.stop.is_set() and time.monotonic() < deadline:
            # STATUS goes to the RAN process, daemon-status to the sidecar: both must answer.
            status = self.kube.injector(["status"])
            if status.startswith("OK") and self.kube.injector(["daemon-status"]).startswith("OK"):
                m = re.search(r"started_us=(\d+)", status)
                if m and int(m.group(1)) > 0:
                    return int(m.group(1)) / 1e6
                self.log("warning", message="RAN process does not report started_us: using now as its start")
                return time.time()
            self.stop.wait(2)
        self.log("restart_failed", step="ran process not answering")
        return None

    def restart(self):
        for attempt in range(1, self.args.restart_retries + 2):
            if self.stop.is_set():
                raise Abort("stopped")
            self.log("restart", attempt=attempt)
            started = self.restart_once()
            if started is not None:
                self.log("ready", attempt=attempt, ran_started_us=int(started * 1e6),
                         exec_latency_s=round(self.kube.exec_latency(), 2))
                return started
        raise Abort(f"pod not ready after {self.args.restart_retries + 1} restarts")

    # -- lease ---------------------------------------------------------------------------------------------------------
    def start_lease(self):
        def renew():
            while not self.stop.is_set() and not self.lease_stop.is_set():
                reply = self.kube.injector(["lease", "--ttl", str(LEASE_TTL_S)], timeout=LEASE_RENEW_S)
                if not reply.startswith("OK"):
                    self.log("lease_renew_failed", quiet=True, reply=reply)
                self.lease_stop.wait(LEASE_RENEW_S)

        self.lease_stop = threading.Event()
        self.lease_thread = threading.Thread(target=renew, name="lease", daemon=True)
        self.lease_thread.start()

    def stop_lease(self):
        if self.lease_thread:
            self.lease_stop.set()
            self.lease_thread.join(timeout=LEASE_RENEW_S + 5)

    # -- campaign ------------------------------------------------------------------------------------------------------
    def wait_until(self, t):
        self.stop.wait(max(0.0, t - time.time()))
        if self.stop.is_set():
            raise Abort("stopped")

    def run_segment(self, index, seg, warmup):
        kind = str(seg.get("type", "")).upper()
        duration = float(seg.get("duration_s", 0))
        if duration <= 0:
            raise Abort(f"segment {index} without a positive duration_s: {seg}")
        if kind == "NONE":
            label = "normal"
        elif kind == "LABEL_ONLY":
            label = seg.get("label", "")
        elif kind == "MULTI":
            anomalies = seg.get("anomalies", [])
            if not anomalies:
                raise Abort(f"segment {index}: MULTI without anomalies")
            label = seg.get("label", "+".join(a.get("type", "").lower() for a in anomalies))
        elif kind in INJECTED_TYPES:
            label = seg.get("label", kind.lower())
        else:
            raise Abort(f"segment {index}: unknown type {kind}")

        self.log("segment_start", index=index, type=kind, label=label, duration_s=duration)
        started = self.restart()
        # aurora_reader records from its first sample + warm-up; the RAN process publishes from `started` on.
        record_start = started + warmup
        record_end = record_start + duration
        # Confirmed before record_start: the lead, plus the time the exec itself takes (and stress-ng start-up).
        self.wait_until(record_start - self.args.inject_lead - self.kube.exec_latency() - 0.5)

        # Lasts until the next restart stops the pod (or, without controller, ends on its own).
        anomaly_ms = int((record_end - time.time() + self.args.margin) * 1000)
        if kind == "LABEL_ONLY":
            reply = self.kube.injector(["label", label, "--duration-ms", str(anomaly_ms)])
        elif kind == "MULTI":
            spec = {"label": label, "duration_ms": anomaly_ms,
                    "anomalies": [dict(a, duration_ms=anomaly_ms) for a in seg["anomalies"]]}
            reply = self.kube.injector(["multi", json.dumps(spec)], timeout=60)
        elif kind != "NONE":
            params = [f"{k}={v}" for k, v in seg.items() if k not in ("type", "duration_s", "label")]
            reply = self.kube.injector(
                ["inject", "--label", label, kind, f"duration_ms={anomaly_ms}"] + params, timeout=60)
        else:
            reply = "OK normal"
        lead = record_start - time.time()
        self.log("inject", index=index, type=kind, label=label, reply=reply, ok=reply.startswith("OK"),
                 anomaly_ms=anomaly_ms if kind != "NONE" else 0, lead_s=round(lead, 2))
        if not reply.startswith("OK"):
            raise Abort(f"segment {index}: {kind} not applied: {reply}")
        # Silent clamp guard: the RAN anomaly module caps SET durations (1 h), so a
        # longer segment would run short and the rest be mislabelled. The reply
        # echoes the applied duration_ms; abort if it is shorter than requested.
        clamp = re.search(r"duration_ms=(\d+)", reply)
        if clamp and kind not in ("NONE", "LABEL_ONLY", "MULTI") and int(clamp.group(1)) < anomaly_ms - 1000:
            raise Abort(f"segment {index}: {kind} duration clamped to {clamp.group(1)} ms < requested "
                        f"{anomaly_ms} ms (RAN anomaly cap); split the segment to <= ~3500 s")
        if lead < 0 and kind != "NONE":
            self.log("warning", message=f"segment {index}: labelled {-lead:.1f} s after the recording started; "
                                        "its first samples are labelled normal")

        self.wait_until(record_end)
        self.log("segment_recorded", index=index, type=kind, label=label,
                 record_start_us=int(record_start * 1e6), record_end_us=int(record_end * 1e6))

    def segments(self, campaign):
        if "random" in campaign:
            yield from random_segments(campaign["random"])
            return
        repeat = int(campaign.get("repeat", 1))
        n = 0
        while repeat == 0 or n < repeat:
            yield from campaign.get("segments", [])
            n += 1

    def run_campaign(self, campaign):
        warmup = self.kube.warmup_s()
        if warmup <= 0:
            self.log("warning", message="AURORA_WARMUP_S not set in the deployment: no warm-up is dropped")
        if warmup < self.args.inject_lead:
            self.log("warning", message="warm-up shorter than --inject-lead: injecting right after the restart")
        self.log("campaign_start", namespace=self.args.namespace, deployment=self.args.deployment,
                 warmup_s=warmup, inject_lead_s=self.args.inject_lead, margin_s=self.args.margin)
        self.start_lease()
        rc = 0
        try:
            for i, seg in enumerate(self.segments(campaign)):
                self.run_segment(i, seg, warmup)
            self.log("campaign_done")
        except Abort as e:
            self.log("campaign_aborted", reason=str(e))
            rc = 1
        finally:
            self.stop_lease()
            was_stopped = self.stop.is_set()
            self.stop.clear()  # the final restart must still run after Ctrl-C
            if not self.args.no_final_restart:
                try:
                    self.restart()
                    self.log("left_normal")
                except Abort as e:
                    self.log("error", message=f"final restart failed: {e}")
                    rc = 1
            if was_stopped:
                rc = 130
        return rc


def pick(rng, value):
    if isinstance(value, (list, tuple)):
        lo, hi = float(value[0]), float(value[-1])
        return rng.uniform(min(lo, hi), max(lo, hi))
    return float(value)


def random_segments(cfg):
    rng = random.Random(cfg.get("seed"))
    types = cfg.get("types", ["MAC_CONTENTION", "PDCP_CONTENTION"])
    count = int(cfg.get("count", 0))
    n = 0
    while count == 0 or n < count:
        yield {"type": "NONE", "duration_s": round(pick(rng, cfg.get("gap_s", [300, 600])))}
        kind = rng.choice(types)
        seg = {"type": kind, "duration_s": round(pick(rng, cfg.get("duration_s", [300, 900])))}
        if kind in ("MAC_CONTENTION", "PDCP_CONTENTION"):
            lo = round(pick(rng, cfg.get("load_min", [5, 30])))
            hi = max(lo, round(pick(rng, cfg.get("load_max", [40, 95]))))
            seg.update(load_min=lo, load_max=hi, step_ms=int(pick(rng, cfg.get("step_ms", 30000))))
            for key in ("slice_ms", "prio_offset", "cell", "method"):
                if key in cfg:
                    seg[key] = cfg[key]
        yield seg
        n += 1


def main():
    parser = argparse.ArgumentParser(description="AURORA anomaly campaign controller (management node)")
    parser.add_argument("-n", "--namespace")
    parser.add_argument("-d", "--deployment", help="RAN Deployment with the anomaly-injector sidecar")
    parser.add_argument("--container", default="anomaly-injector")
    parser.add_argument("--kubectl", default="kubectl")
    parser.add_argument("--context")
    parser.add_argument("--kubeconfig")
    parser.add_argument("--ready-timeout", type=float, default=300, help="per restart attempt (s)")
    parser.add_argument("--restart-retries", type=int, default=5)
    parser.add_argument("--inject-lead", type=float, default=3, help="inject this long before the warm-up ends (s)")
    parser.add_argument("--margin", type=float, default=30, help="anomaly outlasts its segment by this much (s)")
    parser.add_argument("--log", help="JSON-lines log file (appended)")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p_run = sub.add_parser("run", help="run a campaign")
    p_run.add_argument("campaign")
    p_run.add_argument("--no-final-restart", action="store_true", help="leave the pod as it is at the end")
    sub.add_parser("status", help="RAN anomaly module and sidecar status")
    sub.add_parser("restart", help="restart the pod and wait until it is ready")
    p_label = sub.add_parser("label", help="set the label (manual use; no restart)")
    p_label.add_argument("value")
    p_send = sub.add_parser("send", help="send one command line through the sidecar")
    p_send.add_argument("line", nargs="+")
    args = parser.parse_args()

    campaign = None
    if args.cmd == "run":
        with open(args.campaign, encoding="utf-8") as f:
            campaign = json.load(f)
        args.namespace = args.namespace or campaign.get("namespace")
        args.deployment = args.deployment or campaign.get("deployment")
    if not args.namespace or not args.deployment:
        parser.error("namespace and deployment are required (-n/-d or in the campaign file)")
    if args.cmd != "run":
        args.no_final_restart = True

    log = Logger(args.log)
    ctl = Controller(args, log)
    signal.signal(signal.SIGINT, lambda *_: ctl.stop.set())
    signal.signal(signal.SIGTERM, lambda *_: ctl.stop.set())

    if args.cmd == "run":
        return ctl.run_campaign(campaign)
    if args.cmd == "status":
        ran = ctl.kube.injector(["status"])
        daemon = ctl.kube.injector(["daemon-status"])
        log("status", ran=ran, sidecar=daemon)
        return 0 if ran.startswith("OK") and daemon.startswith("OK") else 1
    if args.cmd == "restart":
        try:
            ctl.restart()
            return 0
        except Abort as e:
            log("error", message=str(e))
            return 1
    if args.cmd == "label":
        reply = ctl.kube.injector(["label", args.value])
        log("label", reply=reply)
        return 0 if reply.startswith("OK") else 1
    if args.cmd == "send":
        reply = ctl.kube.injector(["send"] + args.line)
        log("send", reply=reply)
        return 0 if reply.startswith("OK") else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
