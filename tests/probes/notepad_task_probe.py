"""Real Notepad shared-task and close flow on an isolated Wayland/V3D desktop."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2]
    report = {
        "gate": "native-notepad-owner-tasks", "passed": False, "wm_reclaimed": False,
        "expected_scenarios": 4,
        "scope": "actual Notepad DSO, shared tasks/feedback DSL, native input, business IO, WM close; scoped SDK focus request",
        "limitations": ["Square Light only; no FPS, all-theme visual or touch acceptance",
                        "Typed business assertions and file reads are test observations",
                        "Business feedback checks compare the sink's current focus, which may be empty after save/task actions",
                        "Existing native editor focus is tested by a distinct SDK Info request after real Saved and editing; no extra business result is claimed",
                        "Persistent Error deadline is inspected; wall-clock expiry is tested separately",
                        "No change to installed desktop, packages or user files"],
    }
    with tempfile.TemporaryDirectory(prefix="prism-owner-task-notepad-") as runtime:
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
                        raise RuntimeError("Isolated Notepad WM startup failed; see wm.log")
                    time.sleep(0.02)
                native_report = evidence / "probe-results.json"
                command = [str(build / "tests/notepad_task_probe"), env["WAYLAND_DISPLAY"],
                           str(package), str(source), str(native_report)]
                with (evidence / "notepad-task.log").open("w") as output:
                    result = subprocess.run(command, cwd=runtime, env=env, stdout=output,
                                            stderr=subprocess.STDOUT, timeout=120, check=False)
                report.update(command=command, returncode=result.returncode)
                if native_report.exists():
                    report["native"] = json.loads(native_report.read_text(encoding="utf-8"))
                if result.returncode:
                    raise RuntimeError("Notepad native flow failed; see notepad-task.log")
                native = report.get("native", {})
                scenarios = native.get("scenarios", [])
                expected = {"open-save-as", "save-overwrite", "close-cancel-discard", "close-save"}
                if not native.get("passed") or {case["name"] for case in scenarios} != expected:
                    raise RuntimeError("Notepad native report lacks required scenario coverage")
                for case in scenarios:
                    if not all(case.get(field) for field in
                               ("passed", "native_pointer", "native_keyboard", "adopted_tasks",
                                "pixel_commits")):
                        raise RuntimeError("Notepad scenario lacks real input/adoption evidence")
                    if case["name"] != "close-cancel-discard" and not case.get("actual_business_io"):
                        raise RuntimeError("Notepad file scenario lacks actual business IO")
                    if case["name"].startswith("close-") and not case.get("real_wm_close"):
                        raise RuntimeError("Notepad close scenario did not exercise WM close")
                by_name = {case["name"]: case for case in scenarios}
                saved = by_name["open-save-as"]
                recovery = by_name["save-overwrite"]
                if not (saved.get("adopted_feedback") and saved.get("feedback_continued_editing")
                        and saved.get("feedback_focus_unchanged") and saved.get("feedback_focus_checks")):
                    raise RuntimeError("Saved feedback lacks real adoption/focus/continued-editing evidence")
                if not (saved.get("feedback_focus_samples", 0) > 0
                        and saved.get("feedback_focus_preserved")
                        and saved.get("sdk_focus_feedback_adopted")
                        and saved.get("sdk_existing_focus_preserved")
                        and saved.get("sdk_keyboard_without_refocus")):
                    raise RuntimeError("SDK feedback lacks an existing native focus/adoption/keyboard-without-refocus sample")
                if not (recovery.get("adopted_feedback") and recovery.get("native_feedback_actions")
                        and recovery.get("feedback_recovery_retained") and recovery.get("feedback_close_retired")
                        and recovery.get("native_close_events")
                        and recovery.get("feedback_focus_unchanged") and recovery.get("feedback_focus_checks")):
                    raise RuntimeError("Error feedback lacks native recovery/data-retention evidence")
                report["feedback_coverage"] = {
                    "saved_adopted": True, "continued_native_editing": True,
                    "native_change_location": True, "cancel_preserves_draft_and_external_file": True,
                    "failed_real_wm_close_retired_before_recovery": True,
                    "sdk_show_preserves_native_seat_focus": True,
                    "sdk_adoption_preserves_existing_native_editor_focus": True,
                    "native_keyboard_continues_without_refocus": True,
                    "focus_verification_scope": "Distinct test Info request through real Host/SDK after native editor focus; ordinary business Show checks may start unfocused",
                    "focus_samples_with_nonzero_seat": sum(case.get("feedback_focus_samples", 0)
                                                            for case in scenarios),
                }
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
                (evidence / "native-gates.json").write_text(json.dumps(report, indent=2) + "\n",
                                                            encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    args = parser.parse_args()
    print(json.dumps(run(args.build.resolve(), args.evidence.resolve()), indent=2))


if __name__ == "__main__":
    main()
