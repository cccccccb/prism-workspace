"""Interleaved real Music AppHost samples on an isolated V3D compositor.

This does not measure the production launcher pool or OS cold cache. Native
samples use the unchanged built application package; no fixture or work delay.
"""

import argparse
import itertools
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import tempfile
import time


def hardware():
    result = {"thermal": [], "cpufreq": [], "errors": []}
    for directory in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
        try:
            result["thermal"].append({"zone": directory.name,
                                      "type": (directory / "type").read_text().strip(),
                                      "temperature_c": int((directory / "temp").read_text()) / 1000})
        except (OSError, ValueError) as error:
            result["errors"].append(str(error))
    for directory in sorted(Path("/sys/devices/system/cpu/cpufreq").glob("policy*")):
        policy = {"policy": directory.name}
        for name in ("scaling_cur_freq", "scaling_min_freq", "scaling_max_freq", "scaling_governor"):
            try:
                value = (directory / name).read_text().strip()
                policy[name] = int(value) if value.isdigit() else value
            except OSError as error:
                result["errors"].append(str(error))
        result["cpufreq"].append(policy)
    if shutil.which("vcgencmd"):
        try:
            throttle = subprocess.run(["vcgencmd", "get_throttled"], capture_output=True,
                                      text=True, timeout=2, check=False)
            result["throttled_raw"] = throttle.stdout.strip()
            result["throttled_returncode"] = throttle.returncode
        except (OSError, subprocess.TimeoutExpired) as error:
            result["errors"].append(str(error))
    return result


def distribution(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "median": None, "p95": None, "p99": None, "maximum": None}
    return {"count": len(values), "minimum": values[0], "median": statistics.median(values),
            "p95": values[math.ceil(len(values) * .95) - 1],
            "p99": values[math.ceil(len(values) * .99) - 1], "maximum": values[-1]}


def configuration_key(workers, warm, apps):
    return f"workers-{workers}-{'warm' if warm else 'cold'}-apps-{apps}"


def metrics(app):
    stats = app["milestones"]
    result = {"prewarm_us": app["prewarm_us"], "close_us": app["close_us"],
              "frontend_prepare_us": stats["frontend_prepare_us"],
              "preview_prepare_us": stats["preview_prepare_us"],
              "module_load_us": stats["module_load_us"],
              "module_create_us": stats["module_create_us"],
              "pump_processing_max_us": stats["pump_processing_max_us"]}
    for key in ("egl_init_us", "ganesh_init_us", "first_submit_build_us", "first_render_us",
                "first_swap_us"):
        result[key] = stats[key]
    for key in ("master_queued", "preview_submitted", "preview_presented", "master_prepared",
                "master_installed", "master_submitted", "master_presented", "backend_ready",
                "deferred_complete"):
        timestamp = stats[key + "_ns"]
        result["bind_to_" + key + "_us"] = (timestamp - stats["bind_ns"]) / 1000 if timestamp else None
    result["request_to_preview_presented_us"] = (
        stats["preview_presented_ns"] - app["request_ns"]) / 1000
    result["request_to_master_presented_us"] = (
        stats["master_presented_ns"] - app["request_ns"]) / 1000
    result["request_to_all_content_verified_us"] = (
        app["all_content_verified_ns"] - app["request_ns"]) / 1000
    for phase in ("pending", "ready"):
        response = app[phase + "_response"]
        if response["observed"]:
            result[phase + "_caller_return_us"] = (
                response["caller_return_ns"] - response["issued_ns"]) / 1000
            result[phase + "_theme_preflight_us"] = (
                response["applied_ns"] - response["caller_return_ns"]) / 1000
            result[phase + "_control_to_presented_us"] = (
                response["presented_ns"] - response["issued_ns"]) / 1000
            result[phase + "_theme_accepted_to_presented_us"] = (
                response["presented_ns"] - response["applied_ns"]) / 1000
        else:
            result[phase + "_caller_return_us"] = None
            result[phase + "_theme_preflight_us"] = None
            result[phase + "_control_to_presented_us"] = None
            result[phase + "_theme_accepted_to_presented_us"] = None
    memory = app["memory"]
    result["observed_process_pss_peak_kib"] = max((row["pss_kib"] for row in memory), default=0)
    result["observed_process_rss_peak_kib"] = max((row["rss_kib"] for row in memory), default=0)
    result["pss_sampling_total_us"] = sum(row["sampling_us"] for row in memory)
    return result


def aggregate(records):
    groups = {}
    for record in records:
        key = configuration_key(record["workers"], record["warm"], record["app_count"])
        group = groups.setdefault(key, {"native_groups": 0, "app_samples": 0, "failures": 0,
                                       "pending_response_observations": 0, "metrics": {},
                                       "pump_processing_samples_us": [], "startup_pump_processing_samples_us": [],
                                       "sample_group_cpu_us": [], "responsiveness_metrics": {},
                                       "process_peak_rss_kib": []})
        native = record.get("native")
        group["native_groups"] += 1
        group["failures"] += int(not native or not native["passed"])
        if not native:
            continue
        if native["supplementary"]:
            response = native["responsiveness"]
            group["pending_response_observations"] += int(response["pending_response"]["observed"])
            for metric, value in metrics(response).items():
                if value is not None and (metric.startswith("pending_") or metric.startswith("ready_")):
                    group["responsiveness_metrics"].setdefault(metric, []).append(value)
        for sample in native["samples"]:
            usage = sample["process_delta"]
            group["sample_group_cpu_us"].append(usage["user_us"] + usage["system_us"])
            group["process_peak_rss_kib"].append(usage["peak_rss_kib"])
            for app in sample["apps"]:
                group["app_samples"] += 1
                if not app["passed"]:
                    continue
                group["pump_processing_samples_us"].extend(app["pump_processing_us"])
                group["startup_pump_processing_samples_us"].extend(
                    app["pump_processing_us"][:app["startup_pump_count"]])
                for metric, value in metrics(app).items():
                    if value is not None:
                        group["metrics"].setdefault(metric, []).append(value)
    for group in groups.values():
        group["metrics"] = {key: distribution(values) for key, values in group["metrics"].items()}
        group["responsiveness_metrics"] = {key: distribution(values) for key, values in
                                            group["responsiveness_metrics"].items()}
        for key in ("pump_processing_samples_us", "startup_pump_processing_samples_us",
                    "sample_group_cpu_us", "process_peak_rss_kib"):
            group[key] = distribution(group[key])
    return groups


def environment(build):
    result = {"kernel": platform.uname()._asdict(), "build": str(build),
              "cache_policy": "No OS page/GPU cache purge; cold means unprepared AppHost",
              "selected_environment": {key: os.environ.get(key) for key in
                                       ("WLR_RENDER_DRM_DEVICE", "EGL_PLATFORM", "MESA_LOADER_DRIVER_OVERRIDE")}}
    cache = build / "CMakeCache.txt"
    if cache.exists():
        keys = ("CMAKE_BUILD_TYPE:", "CMAKE_CXX_COMPILER:", "PRISM_SKIA_ROOT:")
        result["build_configuration"] = [line for line in cache.read_text().splitlines()
                                           if line.startswith(keys)]
    return result


def run(build, evidence, rounds):
    evidence.mkdir(parents=True, exist_ok=True)
    report = {"gate": "real-music-startup-matrix", "passed": False, "wm_reclaimed": False,
              "environment": environment(build), "hardware_start": hardware(), "runs": [],
              "scope": "Direct Host baseline only; production launcher pool/multi-PID results separate",
              "quantiles": "Nearest-rank empirical quantiles; small samples imply no speed guarantee",
              "memory": "PSS is sampled shared-process high-water, not per-app unique GPU or physical memory",
              "responsiveness": "Separate first-round supplementary host; natural pending only; no pointer/input latency claim",
              "sample_cpu": "Whole timed owner lifecycle including prewarm, final content verification and Close; excludes auxiliary response/cancel",
              "timing": "Pump processing excludes actual blocked poll wait, includes dispatch/render and descheduling; not pure CPU or GPU duration"}
    configurations = list(itertools.product((1, 2), (False, True), (1, 2)))
    try:
        with tempfile.TemporaryDirectory(prefix="prism-real-music-") as runtime:
            root = Path(runtime)
            env = dict(os.environ, XDG_RUNTIME_DIR=runtime, WLR_BACKENDS="headless",
                       WLR_RENDERER="gles2", WAYLAND_DISPLAY="wayland-prism-0")
            env.pop("WAYLAND_SOCKET", None)
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
                            raise RuntimeError("Isolated V3D WM failed to become ready")
                        time.sleep(.02)  # WM bootstrap only; never the native Host event loop.
                    for trial in range(rounds):
                        order = configurations[trial % 8:] + configurations[:trial % 8]
                        if trial % 2:
                            order = list(reversed(order))
                        for workers, warm, apps in order:
                            key = configuration_key(workers, warm, apps)
                            stem = f"trial-{trial}-{key}"
                            record = {"trial": trial, "workers": workers, "warm": warm,
                                      "app_count": apps, "hardware_before": hardware()}
                            native_path = evidence / (stem + ".json")
                            command = [str(build / "tests/demo_startup_probe"), env["WAYLAND_DISPLAY"],
                                       str(build / "share/prism/apps/demo_player"), str(workers),
                                       str(int(warm)), str(apps), "1", str(native_path), str(int(trial == 0))]
                            record["command"] = command
                            report["runs"].append(record)
                            started = time.monotonic()
                            with (evidence / (stem + ".log")).open("w") as output:
                                native = subprocess.run(command, cwd=runtime, env=env, stdout=output,
                                                        stderr=subprocess.STDOUT, timeout=30, check=False)
                            record["returncode"] = native.returncode
                            record["elapsed_seconds"] = time.monotonic() - started
                            if native_path.exists():
                                record["native"] = json.loads(native_path.read_text())
                            if native.returncode or not record.get("native", {}).get("passed"):
                                raise RuntimeError(f"Real Music configuration failed: {stem}")
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
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        report["error"] = str(error)
    finally:
        report["hardware_end"] = hardware()
        report["summary"] = aggregate(report["runs"])
        (evidence / "startup-report.json").write_text(json.dumps(report, indent=2) + "\n",
                                                      encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path)
    parser.add_argument("--evidence", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=5)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 10:
        parser.error("--rounds must be between 1 and 10")
    report = run(args.build.resolve(), args.evidence.resolve(), args.rounds)
    print(json.dumps(report["summary"], indent=2))
    if not report["passed"]:
        raise SystemExit(report.get("error", "Real Music startup matrix failed"))


if __name__ == "__main__":
    main()
