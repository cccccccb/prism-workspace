"""Native V3D async-business gate; all packages and barriers are test-only.

The fixture blocks copied work inputs on cancellable descriptors. No product
sleep, environment delay hook, periodic tick or fallback polling is used.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


INTERFACE = '''
Interface(version:2,layout:"layout.prism") {
    Binding(name:"status",type:"string",initial:"Business starting")
    Binding(name:"progress",type:"number",initial:0)
    Component(id:"main",source:"main.prism",phase:"critical")
}
'''
LAYOUT = '''
Card(background:#18304AFF,padding:18) {
    Slot(component:"main")
}
'''
BODY = '''
VStack(spacing:16) {
    Text($status,font:22,foreground:"@foreground")
    Progress(value:$progress,width:240,height:8,foreground:"@foreground")
    Text("Two owner-managed background jobs",font:16,foreground:"@foreground")
}
'''


def packages(root, fixture):
    for name in ("success", "failure", "cancel"):
        package = root / name
        (package / "assets").mkdir(parents=True)
        (package / "master.prism").write_text(INTERFACE, encoding="utf-8")
        (package / "layout.prism").write_text(LAYOUT, encoding="utf-8")
        (package / "main.prism").write_text(BODY, encoding="utf-8")
        shutil.copyfile(fixture, package / "business.so")
        manifest = {
            "format_version": 1, "runtime_abi": 1,
            "app_id": "prism.business." + name,
            "name": "Async Business " + name, "version": "1",
            "ui": "master.prism", "module": "business.so", "assets": "assets",
            "window": {"width": 480, "height": 320},
        }
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                               encoding="utf-8")


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"build": str(build), "gate": "native-async-business", "passed": False,
              "wm_reclaimed": False, "expected_scenarios": 3,
              "scope": "deterministic work barriers and event waits; no throughput benchmark",
              "limitations": ["No pointer action or resize gate",
                              "dlopen/static constructors/create remain owner entrypoints"]}
    with tempfile.TemporaryDirectory(prefix="prism-async-business-") as runtime:
        root = Path(runtime)
        package_root = root / "packages"
        packages(package_root, build / "tests/libasync_business_fixture.so")
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
                while not (root / env["WAYLAND_DISPLAY"]).exists():
                    if wm.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Isolated GLES WM failed to become ready; see wm.log")
                    time.sleep(0.02)
                native_report = evidence / "probe-results.json"
                command = [str(build / "tests/async_business_probe"),
                           env["WAYLAND_DISPLAY"], str(package_root), str(native_report)]
                started = time.monotonic()
                with (evidence / "async-business.log").open("w") as log:
                    result = subprocess.run(command, cwd=runtime, env=env, stdout=log,
                                            stderr=subprocess.STDOUT, timeout=60, check=False)
                report.update(command=command, returncode=result.returncode,
                              elapsed_seconds=round(time.monotonic() - started, 3))
                if result.returncode:
                    raise RuntimeError("Native async-business gate failed; see async-business.log")
                native = json.loads(native_report.read_text())
                if not native.get("passed") or len(native.get("scenarios", [])) != 3:
                    raise RuntimeError("Native async-business report lacks three passing scenarios")
                report["native"] = native
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
