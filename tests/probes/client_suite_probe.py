"""Manual Pi V3D integration check; never installed with the desktop."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build-gles").resolve()
installed = (build / "bin/prism-wm").exists()
wm_path = build / ("bin/prism-wm" if installed else "prism/prism-wm")
probe_path = (root / "build-gles" if installed else build) / "tests/prism_skia_gles_wayland_probe"
with tempfile.TemporaryDirectory(prefix="prism-suite-") as runtime:
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
               WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
    env.pop("WAYLAND_SOCKET", None)
    processes = []
    working_dir = runtime if installed else root
    log_path = Path(runtime) / "wm.log"
    with log_path.open("w+") as log:
        wm = subprocess.Popen([str(wm_path)], cwd=working_dir,
                              env=env, stdout=log, stderr=log)
        processes.append(wm)
        try:
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                content = log_path.read_text()
                if all(f"authorized shell role={r}" in content for r in (1, 2, 3)):
                    break
                assert wm.poll() is None, content
                time.sleep(0.1)
            else:
                raise AssertionError(log_path.read_text())
            for name in ("demo_player", "demo_settings"):
                processes.append(subprocess.Popen([str(build / ("bin" if installed else "demos") / name)],
                    cwd=working_dir, env=env, stdout=log, stderr=log))
            probe = subprocess.run([str(probe_path),
                "wayland-prism-0", str(root / "tests/fixtures/skia_probe.prism"), "prism_topbar"],
                cwd=root, env=env, capture_output=True, text=True, timeout=25)
            assert probe.returncode == 0, probe.stdout + probe.stderr
            assert "V3D" in probe.stdout, probe.stdout
            content = log_path.read_text()
            assert "Mapped XDG toplevel: Prism Desktop" in content, content
            assert "Mapped XDG toplevel: Prism TopBar" in content, content
            assert "Mapped XDG toplevel: Prism Dock" in content, content
            assert "Mapped XDG toplevel: Prism Music Studio" in content, content
            assert "Mapped XDG toplevel: Prism System Preferences" in content, content
            # The spoofed app_id is followed by credentials showing ordinary role 0.
            assert "Mapped app_id='prism_topbar' shell role=0" in content, content
            assert all(p.poll() is None for p in processes), content
            print("Five client surfaces mapped; spoofed shell app_id stayed ordinary.")
            print(probe.stdout.strip())
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
