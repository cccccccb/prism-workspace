"""Isolated V3D prepared-UI and submission gates; never installed with Prism."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time


def run(build, evidence):
    root = Path(__file__).resolve().parents[2]
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"build": str(build), "gates": [], "wm_reclaimed": False, "passed": False}
    with tempfile.TemporaryDirectory(prefix="prism-prepared-ui-") as runtime:
        env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
                   WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
        env.pop("WAYLAND_SOCKET", None)
        for node in Path("/sys/class/drm").glob("renderD*"):
            driver = node / "device/driver"
            if driver.exists() and driver.resolve().name == "v3d":
                env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name

        with (evidence / "wm.log").open("w") as output:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=runtime, env=env,
                                  stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while not (Path(runtime) / env["WAYLAND_DISPLAY"]).exists():
                    if wm.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Isolated GLES WM did not become ready; see wm.log")
                    time.sleep(0.02)

                commands = [
                    ("prepared-ui", [str(build / "tests/prepared_ui_probe"),
                                     env["WAYLAND_DISPLAY"], str(root / "tests/fixtures")]),
                    ("sdk-submission", [str(build / "tests/prism_skia_gles_wayland_probe"),
                                        env["WAYLAND_DISPLAY"], "--verify-submission",
                                        "prism.preparation.submission"]),
                    ("sdk-damage", [str(build / "tests/prism_skia_gles_wayland_probe"),
                                    env["WAYLAND_DISPLAY"], "--verify-damage",
                                    "prism.preparation.damage"]),
                    ("sdk-animation", [str(build / "tests/prism_skia_gles_wayland_probe"),
                                       env["WAYLAND_DISPLAY"], "--verify-animation",
                                       "prism.preparation.animation"]),
                    ("sdk-input-animation", [str(build / "tests/sdk_input_animation_probe"),
                                             env["WAYLAND_DISPLAY"]]),
                ]
                for name, command in commands:
                    started = time.monotonic()
                    log = evidence / (name + ".log")
                    with log.open("w") as gate_output:
                        result = subprocess.run(command, cwd=runtime, env=env,
                                                stdout=gate_output, stderr=subprocess.STDOUT,
                                                timeout=45, check=False)
                    report["gates"].append({"name": name, "command": command,
                                            "log": str(log), "returncode": result.returncode,
                                            "elapsed_seconds": round(time.monotonic() - started, 3)})
                    if result.returncode:
                        raise RuntimeError(f"{name} failed; see {log}")
                report["passed"] = True
            finally:
                if wm.poll() is None:
                    wm.terminate()
                    try:
                        wm.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        wm.kill()
                        wm.wait(timeout=5)
                report["wm_reclaimed"] = wm.poll() is not None
                (evidence / "native-gates.json").write_text(
                    json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    report = run(args.build.resolve(), args.evidence.resolve())
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
