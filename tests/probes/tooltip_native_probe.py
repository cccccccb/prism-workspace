"""Tooltip input and presentation on an isolated Wayland/V3D Notepad owner."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


SCENARIOS = {
    "hover-focus-and-keyboard", "escape-suppression", "keyboard-focus-and-activation",
    "pointer-click-activation", "file-task-precedence",
}


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2]
    report = {
        "gate": "native-tooltip-input", "passed": False, "wm_reclaimed": False,
        "expected_scenarios": len(SCENARIOS),
        "scope": "actual Notepad DSO, Tooltip DSL, native pointer/keyboard, live focus, shared file task",
        "limitations": [
            "Square Light only; no all-theme visual, touch or FPS acceptance",
            "Live Scene and adopted metadata are test observations, not production hooks",
            "Exact hover deadline arithmetic belongs to deterministic core tests",
            "Business activation is checked through actual Notepad documents; no file IO is claimed",
            "No installed desktop, packages or user files are changed",
        ],
    }
    with tempfile.TemporaryDirectory(prefix="prism-owner-task-tooltip-") as runtime:
        root = Path(runtime)
        package = root / "prism_notepad"
        shutil.copytree(build / "share/prism/apps/prism_notepad", package)
        home = root / "documents"
        home.mkdir()
        env = dict(os.environ, XDG_RUNTIME_DIR=runtime, HOME=str(home),
                   WLR_BACKENDS="headless", WLR_RENDERER="gles2",
                   WAYLAND_DISPLAY="wayland-prism-0")
        env.pop("WAYLAND_SOCKET", None)
        env.pop("PRISMSOCK", None)
        for node in Path("/sys/class/drm").glob("renderD*"):
            driver = node / "device/driver"
            if driver.exists() and driver.resolve().name == "v3d":
                env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name

        with (evidence / "wm.log").open("w") as log:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=runtime, env=env,
                                  stdout=log, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while not (root / env["WAYLAND_DISPLAY"]).exists():
                    if wm.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Tooltip isolated WM startup failed; see wm.log")
                    time.sleep(0.02)
                native_report = evidence / "probe-results.json"
                command = [str(build / "tests/tooltip_native_probe"), env["WAYLAND_DISPLAY"],
                           str(package), str(source), str(native_report)]
                with (evidence / "tooltip-native.log").open("w") as output:
                    result = subprocess.run(command, cwd=runtime, env=env, stdout=output,
                                            stderr=subprocess.STDOUT, timeout=90, check=False)
                report.update(command=command, returncode=result.returncode)
                if native_report.exists():
                    report["native"] = json.loads(native_report.read_text(encoding="utf-8"))
                if result.returncode:
                    raise RuntimeError("Tooltip native probe failed; see tooltip-native.log")
                native = report.get("native", {})
                if set(native.get("scenarios", [])) != SCENARIOS:
                    raise RuntimeError("Tooltip native report lacks required scenario coverage")
                required = (
                    "passed", "actual_notepad_module", "native_hover", "native_keyboard",
                    "readonly_adoption", "viewport_clamped", "existing_focus_preserved",
                    "keyboard_without_refocus", "escape_suppressed", "keyboard_immediate",
                    "native_key_exactly_once", "native_click_exactly_once", "task_precedence",
                    "draft_retained", "focus_samples", "adopted_tooltips", "adopted_tasks",
                    "pixel_commits",
                )
                if not all(native.get(field) for field in required) or native.get("renderer") != "V3D":
                    raise RuntimeError("Tooltip native report lacks real input/adoption/focus/business evidence")
                report["passed"] = True
            except BaseException as error:
                report["error"] = str(error)
                raise
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
    print(json.dumps(run(args.build.resolve(), args.evidence.resolve()), indent=2))


if __name__ == "__main__":
    main()
