"""Isolated V3D Host integration with a cancellable pure-compiler test adapter.

No package, module fixture, probe, or barrier is installed in production.
"""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


PREVIEW = '''
Card(background:#182840FF,padding:16) {
    Text("Preparing Master",font:22,foreground:"@foreground")
}
'''
MASTER = '''
Card(background:#1C324EFF,padding:16) {
    VStack(spacing:12) {
        Text($status,font:22,foreground:"@foreground")
        Button("Master action","action",height:40)
    }
}
'''


def packages(root, fixture):
    cases = {
        "preview": (True, MASTER),
        "master-only": (False, MASTER),
        "default-compiler": (True, 'VStack { Text($status)\n' +
                             '\n'.join('Text("Independent static label")' for _ in range(1000)) +
                             '\n}'),
        "bad-semantic": (True, 'VStack { Text($status) Text("broken",unknownProperty:1) }'),
        "control-priority": (True, 'Text($status,font:22,foreground:"@after_control")'),
        "bad-syntax": (True, 'VStack { Text($status) Text("broken" '),
        "bad-read": (True, MASTER),
        "cancel-preview": (True, MASTER),
        "cancel-master-only": (False, MASTER),
    }
    for name, (preview, master) in cases.items():
        package = root / name
        package.mkdir(parents=True)
        (package / "assets").mkdir()
        (package / "master.prism").write_text(master, encoding="utf-8")
        shutil.copyfile(fixture, package / "business.so")
        manifest = {
            "format_version": 1,
            "runtime_abi": 1,
            "app_id": "prism.async." + name.replace("-", "_"),
            "name": "Async Master " + name,
            "version": "1",
            "ui": "master.prism",
            "module": "business.so",
            "assets": "assets",
            "window": {"width": 480, "height": 320},
        }
        if preview:
            (package / "preview.prism").write_text(PREVIEW, encoding="utf-8")
            manifest["preview"] = "preview.prism"
        (package / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                                encoding="utf-8")


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"build": str(build), "gate": "native-async-master", "passed": False,
              "wm_reclaimed": False,
              "scope": "deterministic compiler barrier, not a latency/performance benchmark"}
    with tempfile.TemporaryDirectory(prefix="prism-async-master-") as runtime:
        runtime_root = Path(runtime)
        package_root = runtime_root / "packages"
        packages(package_root, build / "tests/libasync_master_fixture.so")
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
                while not (runtime_root / env["WAYLAND_DISPLAY"]).exists():
                    if wm.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError("Isolated GLES WM did not become ready; see wm.log")
                    time.sleep(0.02)

                command = [str(build / "tests/async_master_probe"),
                           env["WAYLAND_DISPLAY"], str(package_root)]
                started = time.monotonic()
                with (evidence / "async-master.log").open("w") as gate_output:
                    result = subprocess.run(command, cwd=runtime, env=env, stdout=gate_output,
                                            stderr=subprocess.STDOUT, timeout=90, check=False)
                report.update(command=command, returncode=result.returncode,
                              elapsed_seconds=round(time.monotonic() - started, 3))
                if result.returncode:
                    raise RuntimeError("Async Master Host gate failed; see async-master.log")
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
