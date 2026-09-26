"""Manual Pi launcher/host integration. All fixtures remain under tests/."""
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build-gles").resolve()
MAGIC = 0x50524C31

class Peer:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path))
        self.sock.settimeout(0.2)
        self.buffer = b""
        self.events = []
    def launch(self, request, app, mode=1, fragmented=False):
        encoded = app.encode()
        body = struct.pack(">BH", mode, len(encoded)) + encoded
        frame = struct.pack(">IHHIQQ", MAGIC, 1, 1, len(body), request, 0) + body
        if fragmented:
            for byte in frame:
                self.sock.sendall(bytes([byte]))
                time.sleep(0.001)
        else:
            self.sock.sendall(frame)
    def cancel(self, request):
        self.sock.sendall(struct.pack(">IHHIQQ", MAGIC, 1, 3, 0, request, 0))
    def wait(self, predicate, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate(self.events):
                return self.events
            if len(self.buffer) >= 28:
                magic, version, kind, length, request, instance = struct.unpack(">IHHIQQ", self.buffer[:28])
                assert (magic, version, kind) == (MAGIC, 1, 2)
                if len(self.buffer) >= 28 + length:
                    body = self.buffer[28:28 + length]
                    self.buffer = self.buffer[28 + length:]
                    pid, milestone, error, code, size = struct.unpack(">IBHiH", body[:13])
                    assert len(body) == 13 + size
                    self.events.append(dict(request=request, instance=instance, pid=pid,
                        milestone=milestone, error=error, exit=code, detail=body[13:].decode()))
                    continue
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                continue
            assert data, self.events
            self.buffer += data
        raise AssertionError(self.events)
    def close(self):
        self.sock.close()

def ready(events, request):
    return {4, 5} <= {event["milestone"] for event in events if event["request"] == request}

def exited(events, request):
    return any(e["request"] == request and e["milestone"] == 7 for e in events)

def wait_for(predicate, detail, timeout=15):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError(detail())

def process_identity(pid):
    return Path(f"/proc/{pid}/stat").read_text().split()[21]

with tempfile.TemporaryDirectory(prefix="prism-pool-") as runtime:
    runtime = Path(runtime)
    env = dict(os.environ, XDG_RUNTIME_DIR=str(runtime), WLR_BACKENDS="headless",
        WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
    env.pop("WAYLAND_SOCKET", None)
    for node in Path("/sys/class/drm").glob("renderD*"):
        driver = node / "device/driver"
        if driver.exists() and driver.resolve().name == "v3d":
            env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name
    registry = runtime / "apps"
    registry.mkdir()
    shutil.copytree(build / "share/prism/apps/demo_player", registry / "demo_player")
    for name, module in (("hang", "host_lifecycle_fixture_hang"),
                         ("crash", "host_lifecycle_fixture_crash"),
                         ("parent", "host_lifecycle_fixture_child"),
                         ("bad_abi", "app_module_fixture_wrong_abi")):
        package = registry / name
        package.mkdir()
        (package / "assets").mkdir()
        (package / "master.prism").write_text('VStack(padding: 20, background: #202C40FF) { Text($status, font: 24) }')
        (package / "preview.prism").write_text('VStack(padding: 20, background: #202C40FF) { Text("Loading...", font: 24) }')
        shutil.copyfile(build / "tests" / f"lib{module}.so", package / "module.so")
        (package / "manifest.json").write_text(json.dumps(dict(format_version=1, runtime_abi=1,
            app_id=name, name=f"Fixture {name}", version="1", ui="master.prism", preview="preview.prism",
            module="module.so", assets="assets")))
    wm_log = runtime / "wm.log"
    service_log = runtime / "launcher.log"
    processes = []
    peers = []
    with wm_log.open("w+") as wm_out, service_log.open("w+") as service_out:
        try:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=root,
                env=env, stdout=wm_out, stderr=wm_out)
            processes.append(wm)
            wait_for(lambda: (runtime / "wayland-prism-0").exists(), wm_log.read_text)
            command = [str(build / "bin/prism-launcher"), "--apps-root", str(registry),
                "--pool-size", "1", "--max-workers", "4", "--startup-timeout-ms", "3000"]
            service = subprocess.Popen(command, cwd=runtime, env=env, stdout=service_out, stderr=service_out)
            processes.append(service)
            path = runtime / "prism/launcher.sock"
            wait_for(lambda: "worker ready pid=" in service_log.read_text(), service_log.read_text)
            assert path.stat().st_mode & 0o777 == 0o600
            idle = int(re.findall(r"worker ready pid=(\d+)", service_log.read_text())[-1])
            identity = process_identity(idle)
            pss = re.search(r"^Pss:\s+(\d+) kB", Path(f"/proc/{idle}/smaps_rollup").read_text(), re.M)[1]
            peer = Peer(path); peers.append(peer)
            start = time.monotonic_ns()
            peer.launch(7, "demo_player", fragmented=True)
            peer.wait(lambda events: ready(events, 7))
            events = [e for e in peer.events if e["request"] == 7]
            assert [e["milestone"] for e in events] == [0, 1, 2, 3, 4, 5], events
            assigned = next(e for e in events if e["milestone"] == 1)
            assert assigned["pid"] == idle and process_identity(idle) == identity, events
            assert Path(f"/proc/{idle}/exe").resolve() == build / "bin/prism-app-host"
            print(f"Warm allocation used pre-existing PID {idle}; startup observed {(time.monotonic_ns()-start)/1e6:.1f} ms; idle PSS={pss} KiB.")
            wait_for(lambda: len(re.findall(r"worker ready pid=", service_log.read_text())) >= 2, service_log.read_text)
            prior = len(peer.events)
            peer.launch(7, "demo_player")
            peer.wait(lambda events: len(events) >= prior + 6)
            assert len({e["instance"] for e in peer.events}) == 1, peer.events
            peer.launch(8, "demo_player", mode=0)
            peer.wait(lambda events: any(e["request"] == 8 and e["error"] == 1 for e in events))
            second = Peer(path); peers.append(second)
            second.launch(7, "demo_player")
            second.wait(lambda events: ready(events, 7))
            other = next(e for e in second.events if e["milestone"] == 1)
            assert other["pid"] != idle and other["instance"] != assigned["instance"]
            peer.cancel(7); peer.wait(lambda events: exited(events, 7))
            assert any(e["request"] == 7 and e["error"] == 9 for e in peer.events)
            os.kill(other["pid"], signal.SIGKILL)
            second.wait(lambda events: exited(events, 7))
            assert second.events[-1]["exit"] == -9, second.events
            assert any(e["error"] == 7 for e in second.events), second.events
            print("Request replay, client-local IDs, explicit activation refusal, cancellation and SIGKILL reaping passed.")

            malformed = socket.socket(socket.AF_UNIX)
            malformed.connect(str(path)); malformed.settimeout(2)
            malformed.sendall(struct.pack(">IHHIQQ", MAGIC, 1, 1, 65537, 1, 0))
            assert malformed.recv(1) == b""; malformed.close()
            partial = socket.socket(socket.AF_UNIX); partial.connect(str(path)); partial.sendall(b"PRL1")
            peer.launch(9, "unknown")
            peer.wait(lambda events: any(e["request"] == 9 and e["error"] == 2 for e in events))
            partial.close()
            for request, name, error, code in ((10, "bad_abi", 4, 1), (11, "crash", 7, -11), (12, "hang", 10, -9)):
                peer.launch(request, name)
                peer.wait(lambda events: exited(events, request), timeout=12)
                case = [e for e in peer.events if e["request"] == request]
                assert any(e["error"] == error for e in case), case
                assert case[-1]["exit"] == code, case
                assert not any(e["milestone"] == 5 for e in case), case
            print("Malformed/partial IPC, unknown package, bad ABI, crash and external startup watchdog passed.")

            invocation = subprocess.run([str(build / "bin/prism-invoker"), "demo_player", "--new", "--socket", str(path)],
                cwd=runtime, env=env, capture_output=True, text=True, timeout=15)
            assert invocation.returncode == 0, invocation.stdout + invocation.stderr + service_log.read_text()
            cli_pid = int(re.search(r"pid=(\d+) milestone=1", invocation.stdout)[1])
            os.kill(cli_pid, signal.SIGTERM)
            wait_for(lambda: not Path(f"/proc/{cli_pid}").exists(), service_log.read_text)
            print("Invoker/SDK service path passed; disconnect did not cancel its instance.")

            peer.launch(13, "parent")
            peer.wait(lambda events: ready(events, 13))
            wait_for(lambda: "fixture child milestone=5" in service_log.read_text(), service_log.read_text)
            child = int(re.search(r"fixture child milestone=5 instance=\d+ pid=(\d+) app=demo_player", service_log.read_text())[1])
            peer.cancel(13); peer.wait(lambda events: exited(events, 13))
            assert Path(f"/proc/{child}").exists()
            os.kill(child, signal.SIGTERM)
            wait_for(lambda: not Path(f"/proc/{child}").exists(), service_log.read_text)
            print("Module Launch API and projected child events passed; child instance survives requester exit.")

            peer.launch(14, "demo_player")
            peer.wait(lambda events: ready(events, 14))
            active = next(e["pid"] for e in peer.events if e["request"] == 14 and e["milestone"] == 1)
            service.terminate()
            peer.wait(lambda events: exited(events, 14))
            assert any(e["request"] == 14 and e["error"] == 11 for e in peer.events)
            assert service.wait(timeout=5) == 0, service_log.read_text()
            assert not Path(f"/proc/{active}").exists() and not path.exists()
            print("Session-ended failure, Exited notification and socket/child cleanup passed.")
            for item in peers: item.close()
            peers.clear()

            # The same worker runtime provides a pool=0 baseline.
            cold = subprocess.Popen(command[:-6] + ["--pool-size", "0", "--max-workers", "1", "--startup-timeout-ms", "3000"],
                cwd=runtime, env=env, stdout=service_out, stderr=service_out)
            processes.append(cold)
            wait_for(path.exists, service_log.read_text)
            assert not Path(f"/proc/{cold.pid}/task/{cold.pid}/children").read_text().strip()
            baseline = Peer(path); peers.append(baseline)
            baseline.launch(1, "demo_player")
            baseline.wait(lambda events: ready(events, 1))
            baseline.launch(2, "demo_player")
            baseline.wait(lambda events: any(e["request"] == 2 and e["milestone"] == 0 for e in events))
            baseline.cancel(2)
            baseline.wait(lambda events: any(e["request"] == 2 and e["error"] == 9 for e in events))
            assert not any(e["request"] == 2 and e["pid"] for e in baseline.events), baseline.events
            baseline.cancel(1); baseline.wait(lambda events: exited(events, 1))
            wait_for(lambda: not Path(f"/proc/{cold.pid}/task/{cold.pid}/children").read_text().strip(), service_log.read_text)
            print("Pool=0 baseline, unassigned cancellation and idle shrink passed.")
            # Parent death kills worker, and the stale socket can be reclaimed.
            baseline.launch(3, "demo_player")
            baseline.wait(lambda events: ready(events, 3))
            orphan = next(e["pid"] for e in baseline.events if e["request"] == 3 and e["milestone"] == 1)
            cold.kill(); cold.wait(timeout=5)
            wait_for(lambda: not Path(f"/proc/{orphan}").exists() or Path(f"/proc/{orphan}/stat").read_text().split()[2] == "Z",
                service_log.read_text)
            before = service_log.read_text().count("launcher socket=")
            restarted = subprocess.Popen(command[:-6] + ["--pool-size", "0", "--max-workers", "1", "--startup-timeout-ms", "3000"],
                cwd=runtime, env=env, stdout=service_out, stderr=service_out)
            processes.append(restarted)
            wait_for(lambda: service_log.read_text().count("launcher socket=") > before, service_log.read_text)
            restarted.terminate(); assert restarted.wait(timeout=5) == 0, service_log.read_text()
            print("Unexpected launcher death and stale endpoint restart passed.")
        except Exception:
            print(service_log.read_text(), file=sys.stderr)
            print(wm_log.read_text()[-12000:], file=sys.stderr)
            raise
        finally:
            for peer in peers: peer.close()
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try: process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
