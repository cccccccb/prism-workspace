"""Native V3D owner-task provider gate, isolated from the installed desktop.

The test creates resource-free application packages and separate C fixture DSOs.
Ready/input and callback fences are positive receipts from the real AppHost;
startup polling belongs only to this external test process.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


CASES = ("choice", "cancel", "escape", "business-cancel", "callback-next", "owner-close",
         "owner-one", "owner-two")
FILE_CASES = ("file-open", "file-save-new", "file-save-overwrite", "file-save-changed",
              "file-directory", "file-cancel-stale", "file-owner-close")
BODY = '''
Card(background: #18304AFF, padding: 20, inputShape: "bounds") {
    VStack(spacing: 12) {
        InteractionTarget(action: "owner-action", width: 180, height: 44) {
            Visual(background: #5079B5FF, cornerRadius: 8, justify: "center") {
                Text("Owner action", font: 16, foreground: #FFFFFFFF, anchor: "center")
            }
        }
        Text("Current document", font: 20, foreground: #FFFFFFFF)
        Text("Ordinary controls resume after the task", font: 14, foreground: #BDCDE0FF)
        Card(flex: 1)
    }
}
'''


def packages(root, fixture):
    for name in CASES + FILE_CASES:
        package = root / name
        (package / "assets").mkdir(parents=True)
        (package / "master.prism").write_text(BODY, encoding="utf-8")
        # Distinct file copies ensure two simultaneous owners do not share the
        # native fixture's test-only singleton state through dlopen caching.
        shutil.copyfile(fixture, package / "business.so")
        manifest = {
            "format_version": 1, "runtime_abi": 1,
            "app_id": "prism.task." + name,
            "name": "Owner Task " + name, "version": "1",
            "ui": "master.prism", "module": "business.so", "assets": "assets",
            "window": {"width": 640, "height": 420},
        }
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                               encoding="utf-8")
        if name in FILE_CASES:
            directory = root.parent / "files" / name / "input"
            (directory / "folder").mkdir(parents=True)
            (directory / "existing.md").write_text("Existing document.\n", encoding="utf-8")
            (directory / "old.md").write_text("Open document.\n", encoding="utf-8")
            (directory / "ignored.txt").write_text("Excluded extension.\n", encoding="utf-8")
            (directory / ".hidden.md").write_text("Hidden document.\n", encoding="utf-8")
            (directory / "folder" / "inner.md").write_text("Nested document.\n", encoding="utf-8")


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2]
    report = {
        "build": str(build), "gate": "native-owner-task-provider", "passed": False,
        "wm_reclaimed": False, "expected_scenarios": 14,
        "scope": "real Host/shared DSL panel/V3D worker adoption/native pointer and keyboard",
        "limitations": ["No throughput benchmark, visual approval or touch input",
                        "Stale descriptors are injected through the SDK event gate after real adoption",
                        "Busy hints are pure Host projections, without synthetic phases submitted to the GPU",
                        "File service returns selections and does not perform business reads or writes"],
    }
    with tempfile.TemporaryDirectory(prefix="prism-owner-task-") as runtime:
        root = Path(runtime)
        package_root = root / "packages"
        packages(package_root, build / "tests/libowner_task_provider_fixture.so")
        env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
                   WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
        env.pop("WAYLAND_SOCKET", None)
        env.pop("PRISMSOCK", None)
        for node in Path("/sys/class/drm").glob("renderD*"):
            driver = node / "device/driver"
            if driver.exists() and driver.resolve().name == "v3d":
                env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name

        with (evidence / "wm.log").open("w") as output:
            wm = subprocess.Popen([str(build / "bin/prism-wm")], cwd=runtime, env=env,
                                  stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 15
                while not (root / env["WAYLAND_DISPLAY"]).exists():
                    if wm.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Isolated GLES WM failed to become ready; see wm.log")
                    time.sleep(0.02)
                native_report = evidence / "probe-results.json"
                command = [str(build / "tests/owner_task_provider_probe"),
                           env["WAYLAND_DISPLAY"], str(package_root), str(source),
                           str(native_report)]
                started = time.monotonic()
                with (evidence / "owner-task-provider.log").open("w") as log:
                    result = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                            stderr=subprocess.STDOUT, timeout=240, check=False)
                report.update(command=command, returncode=result.returncode,
                              elapsed_seconds=round(time.monotonic() - started, 3))
                if native_report.exists():
                    report["native"] = json.loads(native_report.read_text(encoding="utf-8"))
                if result.returncode:
                    raise RuntimeError("Native owner-task gate failed; see owner-task-provider.log")
                native = report["native"]
                if not native.get("passed") or len(native.get("scenarios", [])) != 14:
                    raise RuntimeError("Native owner-task report lacks fourteen passing scenarios")
                expected = set(CASES[:6]) | {"two-owners"} | set(FILE_CASES)
                scenarios = native["scenarios"]
                if {case["name"] for case in scenarios} != expected:
                    raise RuntimeError("Native owner-task report has incorrect scenario coverage")
                for case in scenarios:
                    if not case.get("passed") or not case.get("owner_isolated"):
                        raise RuntimeError("Native owner-task report contains a failed scenario")
                    if case["name"] != "business-cancel":
                        adopted = case.get("adopted", [])
                        if not adopted or any(not stamp.get(field) for stamp in adopted
                                              for field in ("owner", "request", "epoch",
                                                            "input_version", "frame_sequence",
                                                            "pixel_commits")):
                            raise RuntimeError("Native task Ready lacks an actual adoption receipt")
                    if case["name"] in FILE_CASES:
                        if not all(case.get(field) for field in
                                   ("real_pointer", "file_fd_drained", "filesystem_no_write",
                                    "complete_path_caption", "busy_hints_checked",
                                    "pure_host_projection")):
                            raise RuntimeError("File task lacks real pointer, drained model FD or no-write evidence")
                        expected_callbacks = {"file-owner-close": 0,
                                              "file-cancel-stale": 2}.get(case["name"], 1)
                        if case.get("callbacks") != expected_callbacks:
                            raise RuntimeError("File task callback count is incorrect")
                        if (case["name"] in ("file-save-new", "file-cancel-stale") and
                                not case.get("real_keyboard")):
                            raise RuntimeError("File task lacks native filename or Escape keyboard evidence")
                        if (case["name"] in ("file-save-overwrite", "file-save-changed") and
                                not case.get("overwrite_revalidated")):
                            raise RuntimeError("Save task bypassed overwrite revalidation")
                        if (case["name"] == "file-cancel-stale" and
                                not case.get("stale_input_rejected")):
                            raise RuntimeError("File navigation lacks old-descriptor rejection evidence")
                        paths = case.get("file_paths", [])
                        if expected_callbacks == 1 and (len(paths) != 1 or
                                                        not Path(paths[0]).is_absolute()):
                            raise RuntimeError("Successful file task lacks a canonical absolute selection")
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
