"""Measure the real Music package through cold/warm production launcher workers.

Received launch milestones are observer times; FirstPresented belongs to Preview.
Master/all-content verification is measured separately by demo_startup_probe.
"""

import argparse
import json
import math
import os
from pathlib import Path
import re
import select
import socket
import struct
import subprocess
import tempfile
import time


MAGIC = 0x50524C31


class Peer:
    def __init__(self, path, sample):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path))
        self.buffer = b""
        self.events = []
        self.sample = sample

    def send(self, kind, request, body=b""):
        self.sock.sendall(struct.pack(">IHHIQQ", MAGIC, 1, kind, len(body), request, 0) + body)

    def launch(self, request):
        app = b"demo_player"
        self.send(1, request, struct.pack(">BH", 1, len(app)) + app)

    def wait(self, predicate, cancelling=False):
        deadline = time.monotonic() + 15
        while not predicate():
            self.sample()
            if time.monotonic() >= deadline:
                raise RuntimeError(f"Launcher milestone deadline: {self.events}")
            if len(self.buffer) >= 28:
                magic, version, kind, length, request, instance = struct.unpack(
                    ">IHHIQQ", self.buffer[:28])
                if (magic, version, kind) != (MAGIC, 1, 2) or length > 65536:
                    raise RuntimeError("Unexpected launcher response")
                if len(self.buffer) >= 28 + length:
                    body = self.buffer[28:28 + length]
                    self.buffer = self.buffer[28 + length:]
                    pid, milestone, error, code, size = struct.unpack(">IBHiH", body[:13])
                    if len(body) != 13 + size:
                        raise RuntimeError("Malformed launch event")
                    self.events.append(dict(request=request, instance=instance, pid=pid,
                                            milestone=milestone, error=error, exit=code,
                                            detail=body[13:].decode(), received_ns=time.monotonic_ns()))
                    if milestone == 6 and not (cancelling and error == 9):
                        raise RuntimeError(f"Application launch failed: {self.events[-1]}")
                    continue
            if select.select([self.sock], [], [], .05)[0]:
                data = self.sock.recv(65536)
                if not data:
                    raise RuntimeError("Launcher closed before requested milestones")
                self.buffer += data

    def has(self, request, milestone):
        return any(e["request"] == request and e["milestone"] == milestone for e in self.events)


def environment():
    result = {}
    for name, path in (("temperature_millic", "/sys/class/thermal/thermal_zone0/temp"),
                       ("frequency_khz", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"),
                       ("governor", "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")):
        try:
            result[name] = Path(path).read_text().strip()
        except OSError:
            result[name] = None
    return result


class SessionSamples:
    def __init__(self, wm, launcher):
        self.wm = wm
        self.launcher = launcher
        self.first_ticks = {}
        self.last_ticks = {}
        self.children_seen = set()
        self.peak_sampled_pss_kib = 0
        self.last_sample = 0
        self.initial = True

    def collect(self, force=False):
        now = time.monotonic()
        if not force and now - self.last_sample < .1:
            return
        self.last_sample = now
        try:
            children = [int(p) for p in Path(
                f"/proc/{self.launcher}/task/{self.launcher}/children").read_text().split()]
        except OSError:
            children = []
        self.children_seen.update(children)
        total_pss = 0
        for pid in [self.wm, self.launcher] + children:
            try:
                raw = Path(f"/proc/{pid}/stat").read_text()
                fields = raw[raw.rfind(")") + 2:].split()
                identity = (pid, fields[19])
                ticks = int(fields[11]) + int(fields[12])
                # Newly born workers contribute CPU from process birth.
                self.first_ticks.setdefault(identity, ticks if self.initial else
                                            (0 if pid in children else ticks))
                self.last_ticks[identity] = ticks
                match = re.search(r"^Pss:\s+(\d+) kB", Path(
                    f"/proc/{pid}/smaps_rollup").read_text(), re.M)
                if match:
                    total_pss += int(match[1])
            except OSError:
                continue
        self.peak_sampled_pss_kib = max(self.peak_sampled_pss_kib, total_pss)
        self.initial = False

    def cpu_ms(self):
        return sum(ticks - self.first_ticks[key] for key, ticks in self.last_ticks.items()) * \
            1000 / os.sysconf("SC_CLK_TCK")


def wait_for(predicate, processes):
    deadline = time.monotonic() + 15
    while not predicate():
        if any(p.poll() is not None for p in processes) or time.monotonic() >= deadline:
            raise RuntimeError("Isolated production launcher preparation failed")
        time.sleep(.02)


def one(build, evidence, active, warm, apps, round_number):
    name = f"r{round_number}-active{active}-pool{apps if warm else 0}-apps{apps}"
    directory = evidence / name
    directory.mkdir(parents=True, exist_ok=True)
    result = dict(name=name, active_limit=active, warm=warm, apps=apps, passed=False,
                  environment_before=environment())
    processes = []
    peer = None
    samples = None
    with tempfile.TemporaryDirectory(prefix="prism-music-launch-") as runtime:
        env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
                   WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
        env.pop("WAYLAND_SOCKET", None)
        for node in Path("/sys/class/drm").glob("renderD*"):
            driver = node / "device/driver"
            if driver.exists() and driver.resolve().name == "v3d":
                env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name
        with (directory / "wm.log").open("w") as wm_log, \
                (directory / "launcher.log").open("w") as launcher_log:
            try:
                wm = subprocess.Popen([str(build / "bin/prism-wm")], env=env, cwd=runtime,
                                      stdout=wm_log, stderr=subprocess.STDOUT)
                processes.append(wm)
                wait_for(lambda: (Path(runtime) / env["WAYLAND_DISPLAY"]).exists(), processes)
                launcher = subprocess.Popen([
                    str(build / "bin/prism-launcher"), "--apps-root",
                    str(build / "share/prism/apps"), "--pool-size", str(apps if warm else 0),
                    "--max-workers", str(apps + (apps if warm else 0)),
                    "--load-active-limit", str(active), "--startup-timeout-ms", "10000"],
                    env=env, cwd=runtime, stdout=launcher_log, stderr=subprocess.STDOUT)
                processes.append(launcher)
                endpoint = Path(runtime) / "prism/launcher.sock"
                wait_for(endpoint.exists, processes)
                if warm:
                    wait_for(lambda: len(re.findall(r"worker ready pid=", (
                        directory / "launcher.log").read_text())) >= apps, processes)
                    wait_for(lambda: len(re.findall(r"worker theme installed .*bound=0", (
                        directory / "launcher.log").read_text())) >= apps, processes)
                samples = SessionSamples(wm.pid, launcher.pid)
                samples.collect(True)
                result["preexisting_workers"] = sorted(samples.children_seen)
                peer = Peer(endpoint, samples.collect)
                started = time.monotonic_ns()
                for request in range(1, apps + 1):
                    peer.launch(request)
                peer.wait(lambda: all(peer.has(r, 4) and peer.has(r, 5)
                                      for r in range(1, apps + 1)))
                samples.collect(True)
                result.update(elapsed_ms=(time.monotonic_ns() - started) / 1e6,
                              session_cpu_ms=samples.cpu_ms(),
                              peak_sampled_session_pss_kib=samples.peak_sampled_pss_kib,
                              events=peer.events.copy())
                result["milestones_ms"] = [dict(
                    request=r, assigned=next((e["received_ns"] - started) / 1e6
                                              for e in peer.events if e["request"] == r and e["milestone"] == 1),
                    preview_presented=next((e["received_ns"] - started) / 1e6
                                           for e in peer.events if e["request"] == r and e["milestone"] == 4),
                    backend_ready=next((e["received_ns"] - started) / 1e6
                                       for e in peer.events if e["request"] == r and e["milestone"] == 5))
                    for r in range(1, apps + 1)]
                for r in range(1, apps + 1):
                    if not next(e for e in peer.events if e["request"] == r and e["milestone"] == 4)["detail"].startswith("GL renderer=V3D"):
                        raise RuntimeError("Expected actual V3D first presentation")
                    peer.send(3, r)
                peer.wait(lambda: all(peer.has(r, 7) for r in range(1, apps + 1)), cancelling=True)
                result["cancelled_and_exited"] = True
                result["passed"] = True
            finally:
                if peer:
                    peer.sock.close()
                for process in reversed(processes):
                    if process.poll() is None:
                        process.terminate()
                        try:
                            process.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            process.kill()
                            process.wait(timeout=5)
                result["processes_reclaimed"] = all(p.poll() is not None for p in processes)
                remaining = []
                if samples:
                    deadline = time.monotonic() + 5
                    while time.monotonic() < deadline:
                        remaining = [pid for pid in samples.children_seen
                                     if Path(f"/proc/{pid}").exists()]
                        if not remaining:
                            break
                        time.sleep(.02)
                result["remaining_worker_pids"] = remaining
                result["workers_reclaimed"] = not remaining
                result["passed"] = result["passed"] and not remaining and result["processes_reclaimed"]
                result["environment_after"] = environment()
                (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n")
                if remaining:
                    raise RuntimeError(f"Workers survived isolated session cleanup: {remaining}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=3)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 20:
        parser.error("rounds must be 1..20")
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    configurations = [(active, warm, apps) for active in (1, 2)
                      for warm in (False, True) for apps in (1, 2)]
    results = []
    for iteration in range(args.rounds):
        order = configurations[iteration:] + configurations[:iteration]
        if iteration % 2:
            order = list(reversed(order))
        for active, warm, apps in order:
            results.append(one(args.build.resolve(), evidence, active, warm, apps, iteration + 1))
            (evidence / "samples.json").write_text(json.dumps(results, indent=2) + "\n")
    summary = []
    for active, warm, apps in configurations:
        group = [r for r in results if (r["active_limit"], r["warm"], r["apps"]) == (active, warm, apps)]
        times = sorted(r["elapsed_ms"] for r in group)
        summary.append(dict(active_limit=active, warm=warm, apps=apps, samples=len(times),
                            median_ms=times[len(times) // 2], p95_ms=times[math.ceil(.95 * len(times)) - 1],
                            max_sampled_pss_kib=max(r["peak_sampled_session_pss_kib"] for r in group),
                            failures=sum(not r["passed"] for r in group)))
    report = dict(passed=all(r["passed"] for r in results), groups=len(results), summary=summary,
                  limitations=["Small sample; p95 is an order statistic, not a stable population estimate",
                               "Linux page caches retained; cold means no preexisting Host",
                               "FirstPresented is Preview, not Master; milestone receipt is observed IPC time",
                               "CPU ticks/PSS sampled across WM, launcher and workers including pool refill",
                               "Headless V3D with physical desktop still active; not DRM input latency"])
    (evidence / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
