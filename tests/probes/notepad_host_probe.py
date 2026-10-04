"""Isolated real compositor/Host startup smoke; never replaces the user session."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

build = Path(sys.argv[1] if len(sys.argv) > 1 else "build-gles").resolve()
with tempfile.TemporaryDirectory(prefix="prism-notepad-host-") as directory:
    runtime = Path(directory)
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_BACKENDS="headless",
               WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
    env.pop("WAYLAND_SOCKET", None)
    processes = []
    with (runtime / "wm.log").open("w+") as wm_log, (runtime / "host.log").open("w+") as host_log:
        try:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=directory,
                                  env=env, stdout=wm_log, stderr=wm_log)
            processes.append(wm)
            deadline = time.monotonic() + 20
            while not (runtime / "wayland-prism-0").exists():
                assert wm.poll() is None, (runtime / "wm.log").read_text()
                assert time.monotonic() < deadline, "Compositor startup timed out"
                time.sleep(0.05)
            host = subprocess.Popen([str(build / "bin/prism-app-host"), "--package",
                                     str(build / "share/prism/apps/prism_notepad")],
                                    cwd=directory, env=env, stdout=host_log, stderr=host_log)
            processes.append(host)
            deadline = time.monotonic() + 30
            while "event=BackendReady" not in (runtime / "host.log").read_text():
                text = (runtime / "host.log").read_text()
                assert host.poll() is None, text
                assert time.monotonic() < deadline, text
                time.sleep(0.05)
            time.sleep(0.2)
            assert host.poll() is None
            print((runtime / "host.log").read_text())
            assert "Mapped app_id='prism_notepad' shell role=0" in (runtime / "wm.log").read_text()
            print("PASS: Notepad mapped and BackendReady on an isolated Wayland compositor")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
