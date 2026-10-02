"""Isolated V3D mouse → real Topbar/Host/launcher → WM group-mode regression."""

import argparse
from dataclasses import asdict, dataclass
import json
import os
from pathlib import Path
import re
import select
import signal
import subprocess
import tempfile
import time


@dataclass(frozen=True)
class Rect:
    x: float
    y: float
    width: float
    height: float


@dataclass(frozen=True)
class View:
    node: int
    app: str
    instance: int
    pid: int
    visible: bool
    fullscreen: bool
    rect: Rect
    committed: Rect


def decode_views(document):
    """Decode IPC JSON once; assertions below use the resulting value objects."""
    def visit(node):
        if node["type"] == "view":
            result.append(View(node["id"], node["app_id"], node["instance"], node["pid"],
                               node["visible"], node["fullscreen"], Rect(**node["rect"]),
                               Rect(**node["committed_rect"])))
        for child in node.get("nodes", []):
            visit(child)

    result = []
    for workspace in document["workspaces"]:
        if workspace["active"]:
            visit(workspace)
    return tuple(sorted(result, key=lambda view: view.app))


def children(pid):
    path = Path(f"/proc/{pid}/task/{pid}/children")
    return [int(child) for child in path.read_text().split()] if path.exists() else []


def wait_for(function, description, timeout=15):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = function()
        if last:
            return last
        time.sleep(.05)
    raise AssertionError(f"Timed out: {description}; last={last}")


def line_from(process, timeout=10):
    ready, _, _ = select.select([process.stdout], [], [], timeout)
    if not ready:
        raise AssertionError("Pointer probe did not respond within its bounded command timeout")
    line = process.stdout.readline().strip()
    if not line:
        raise AssertionError(f"Pointer probe exited: {process.poll()}")
    return line


def command(process, name, transcript):
    assert name in ("gesture", "edge-click")
    process.stdin.write(name + "\n")
    process.stdin.flush()
    answer = line_from(process)
    transcript.write(answer + "\n")
    transcript.flush()
    assert answer == "DONE " + name, answer


def expanded(views, width, height):
    if len(views) != 2:
        return False
    first, second = (view.rect for view in views)
    separated = (first.x + first.width <= second.x or second.x + second.width <= first.x
                 or first.y + first.height <= second.y or second.y + second.height <= first.y)
    return (len(views) == 2 and all(view.visible and not view.fullscreen for view in views)
            and all(view.rect == view.committed for view in views)
            and min(view.rect.x for view in views) == 0
            and min(view.rect.y for view in views) == 0
            and max(view.rect.x + view.rect.width for view in views) == width
            and max(view.rect.y + view.rect.height for view in views) == height
            and separated)


def run(build, evidence):
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"build": str(build), "passed": False, "reclaimed": False, "cycles": []}
    pointer = None
    pids = []
    with tempfile.TemporaryDirectory(prefix="prism-group-session-") as directory:
        runtime = Path(directory)
        env = dict(os.environ, XDG_RUNTIME_DIR=directory, WLR_BACKENDS="headless",
                   WLR_RENDERER="gles2", WLR_HEADLESS_OUTPUTS="1",
                   WAYLAND_DISPLAY="wayland-prism-0")
        for variable in ("WAYLAND_SOCKET", "PRISMSOCK", "DISPLAY"):
            env.pop(variable, None)
        for node in Path("/sys/class/drm").glob("renderD*"):
            driver = node / "device/driver"
            if driver.exists() and driver.resolve().name == "v3d":
                env["WLR_RENDER_DRM_DEVICE"] = "/dev/dri/" + node.name

        log_path = evidence / "session.log"
        with log_path.open("w") as log, (evidence / "pointer.log").open("w") as pointer_log:
            session = subprocess.Popen(
                [str(build / "bin/prism-session-runtime"), "--wm", str(build / "bin/prism-wm"),
                 "--apps-root", str(build / "share/prism/apps"),
                 "--themes-root", str(build / "share/prism/themes"), "--theme", "glass"],
                cwd=directory, env=env, stdout=log, stderr=subprocess.STDOUT,
                start_new_session=True)
            try:
                def booted():
                    assert session.poll() is None, log_path.read_text()
                    content = log_path.read_text()
                    return all(f"Mapped app_id='{app}' shell role={role}" in content
                               for app, role in (("prism_desktop", 1), ("prism_topbar", 2),
                                                 ("prism_dock", 3)))

                wait_for(booted, "trusted Shells mapped", 20)
                match = re.search(r"session wm=(\d+) launcher=(\d+)", log_path.read_text())
                assert match, log_path.read_text()
                wm, launcher = map(int, match.groups())
                pids = [wm, launcher] + children(launcher)
                for app in ("demo_player", "demo_settings"):
                    launched = subprocess.run([str(build / "bin/prism-invoker"), app],
                                              cwd=directory, env=env, capture_output=True,
                                              text=True, timeout=20)
                    (evidence / (app + ".log")).write_text(launched.stdout + launched.stderr)
                    assert launched.returncode == 0, launched.stdout + launched.stderr

                def views():
                    assert session.poll() is None, log_path.read_text()
                    result = subprocess.run(
                        [str(build / "prism/prism-msg"), "-s", str(runtime / "prism-ipc.sock"),
                         "get_tree"], cwd=directory, env=env, capture_output=True, text=True,
                        check=True, timeout=5)
                    current = decode_views(json.loads(result.stdout))
                    report["last_views"] = [asdict(view) for view in current]
                    return current

                pointer = subprocess.Popen(
                    [str(build / "tests/group_pointer_probe"), env["WAYLAND_DISPLAY"],
                     str(build / "share/prism/themes"),
                     str(build / "share/prism/apps/prism_topbar/master.prism")],
                    cwd=directory, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                    stderr=pointer_log, text=True, bufsize=1)
                ready = line_from(pointer)
                pointer_log.write(ready + "\n")
                pointer_log.flush()
                values = ready.split()
                assert len(values) == 8 and values[0] == "READY", ready
                width, height, handle_x, handle_y, top, bottom, gap = map(float, values[1:])
                report["pointer"] = {"width": width, "height": height, "handle_x": handle_x,
                                     "handle_y": handle_y, "top": top, "bottom": bottom,
                                     "outer_gap": gap}

                def normal_ready():
                    current = views()
                    if len(current) != 2 or {view.app for view in current} != {
                            "demo_player", "demo_settings"}:
                        return False
                    if not all(view.visible and not view.fullscreen and
                               view.rect == view.committed for view in current):
                        return False
                    assert min(view.rect.y for view in current) == top + gap, current
                    assert max(view.rect.y + view.rect.height for view in current) == (
                        height - bottom - gap), current
                    return current

                baseline = wait_for(normal_ready, "two real demos committed their normal BSP")
                report["baseline"] = [asdict(view) for view in baseline]
                for cycle in range(2):
                    report["stage"] = f"enter-{cycle + 1}"
                    command(pointer, "gesture", pointer_log)

                    def immersive_ready():
                        current = views()
                        return current if expanded(current, width, height) else False

                    immersive = wait_for(immersive_ready, "real group gesture expanded both demos")
                    assert [(view.node, view.instance, view.pid) for view in immersive] == [
                        (view.node, view.instance, view.pid) for view in baseline]
                    report["stage"] = f"edge-click-{cycle + 1}"
                    command(pointer, "edge-click", pointer_log)
                    assert views() == immersive, "Recovery reveal must preserve immersive geometry"
                    report["stage"] = f"restore-{cycle + 1}"
                    command(pointer, "gesture", pointer_log)
                    wait_for(lambda: views() == baseline, "real recovery gesture restored BSP")
                    report["cycles"].append({"cycle": cycle + 1,
                                              "immersive": [asdict(view) for view in immersive],
                                              "restored": True})
                report["behavior_passed"] = True
                report["stage"] = "complete"
            finally:
                if pointer is not None:
                    if pointer.poll() is None:
                        try:
                            pointer.stdin.write("quit\n")
                            pointer.stdin.flush()
                        except BrokenPipeError:
                            pass
                        try:
                            pointer.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            pointer.kill()
                            pointer.wait(timeout=5)
                    try:
                        pointer.stdin.close()
                    except BrokenPipeError:
                        pass
                    pointer.stdout.close()
                if pids:
                    pids = list(set(pids + children(pids[1])))
                if session.poll() is None:
                    session.terminate()
                    try:
                        session.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(session.pid, signal.SIGKILL)
                        session.wait(timeout=5)
                deadline = time.monotonic() + 5
                while any(Path(f"/proc/{pid}").exists() for pid in pids) and time.monotonic() < deadline:
                    time.sleep(.05)
                report["reclaimed"] = all(not Path(f"/proc/{pid}").exists() for pid in pids)
                report["session_exit"] = session.returncode
                report["passed"] = (report.get("behavior_passed", False) and report["reclaimed"]
                                    and report["session_exit"] == 0)
                (evidence / "group-session.json").write_text(json.dumps(report, indent=2) + "\n")
    assert report["reclaimed"] and report["session_exit"] == 0, report
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(run(args.build.resolve(), args.evidence.resolve()), indent=2))


if __name__ == "__main__":
    main()
