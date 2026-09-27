"""Manual common-host integration on Pi; no production install rules."""
import json
import shutil
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else root / "build-gles").resolve()
with tempfile.TemporaryDirectory(prefix="prism-host-") as runtime:
    env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
               WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
    env.pop("WAYLAND_SOCKET", None)
    # Prefer the hardware driver for EGL as in the installed session script.
    for node in Path("/sys/class/drm").glob("renderD*"):
        driver = node / "device/driver"
        if driver.exists() and driver.resolve().name == "v3d":
            env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name
    processes = []
    wm_log = Path(runtime) / "wm.log"
    host_log = Path(runtime) / "host.log"
    with wm_log.open("w+") as wm_out, host_log.open("w+") as host_out:
        try:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=root,
                env=env, stdout=wm_out, stderr=wm_out)
            processes.append(wm)
            deadline = time.monotonic() + 15
            while not (Path(runtime) / "wayland-prism-0").exists():
                assert wm.poll() is None, wm_log.read_text()
                assert time.monotonic() < deadline, wm_log.read_text()
                time.sleep(0.05)
            host = subprocess.Popen([str(build / "bin/prism-app-host"), "--package",
                str(build / "share/prism/apps/demo_player"), "--request-id", "71", "--instance-id", "81"],
                cwd=runtime, env=env, stdout=host_out, stderr=host_out)
            processes.append(host)
            deadline = time.monotonic() + 20
            while True:
                content = host_log.read_text()
                if "event=BackendReady" in content:
                    break
                assert host.poll() is None, content + wm_log.read_text()
                assert time.monotonic() < deadline, content + wm_log.read_text()
                time.sleep(0.05)
            events = [line.split("event=")[1].split()[0]
                      for line in content.splitlines() if line.startswith("host event=")]
            assert events == ["RuntimeReady", "SurfaceConfigured", "FirstPresented", "BackendReady"], content
            assert "request=71 instance=81" in content, content
            assert wm_log.read_text().count("Mapped app_id='demo_player' shell role=0") == 1, wm_log.read_text()
            time.sleep(0.7)  # Exercise the business timer on the real host loop.
            assert host.poll() is None, host_log.read_text()
            host.terminate()
            assert host.wait(timeout=5) == 0, host_log.read_text()
            print(content.strip())
            print("Preview presented before business ready; master reused one surface; graceful stop passed.")
            # Exercise independent BackendReady ordering without a Preview.
            for name, bad_module in (("master_only", False), ("wrong_abi", True)):
                package = Path(runtime) / name
                shutil.copytree(build / "share/prism/apps/demo_player", package)
                manifest_file = package / "manifest.json"
                manifest = json.loads(manifest_file.read_text())
                if bad_module:
                    shutil.copyfile(build / "tests/libapp_module_fixture_wrong_abi.so", package / "player.so")
                else:
                    manifest.pop("preview")
                manifest_file.write_text(json.dumps(manifest))
                case_log = Path(runtime) / (name + ".log")
                with case_log.open("w+") as case_out:
                    case = subprocess.Popen([str(build / "bin/prism-app-host"), "--package", str(package)],
                        cwd=runtime, env=env, stdout=case_out, stderr=case_out)
                    processes.append(case)
                    deadline = time.monotonic() + 15
                    while True:
                        case_content = case_log.read_text()
                        if bad_module and case.poll() is not None:
                            assert case.returncode == 1, case_content
                            assert "event=FirstPresented" in case_content, case_content
                            assert "event=Failed" in case_content and "error=4" in case_content, case_content
                            assert "event=BackendReady" not in case_content, case_content
                            break
                        if not bad_module and "event=FirstPresented" in case_content:
                            assert case_content.index("event=BackendReady") < case_content.index("event=FirstPresented"), case_content
                            case.terminate()
                            assert case.wait(timeout=5) == 0, case_content
                            break
                        assert case.poll() is None, case_content
                        assert time.monotonic() < deadline, case_content
                        time.sleep(0.05)
            print("Master-only readiness ordering and post-Preview ABI failure passed.")

        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
