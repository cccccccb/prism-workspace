#!/usr/bin/env python3
"""Observe an installed Prism session without launching clients or changing it.

Example: sudo python3 tests/probes/performance_session_probe.py \
    --output dist/validation/performance/idle --label idle --seconds 12
Set the desired idle/music/pointer/theme condition before collecting a label.
CPU 100% means one core; a four-core Pi has a 400% aggregate ceiling. WM timing
percentiles are rolling compositor windows, not whole-run latency percentiles.
"""
import argparse
import datetime
import getpass
import json
import os
from pathlib import Path
import pwd
import re
import shutil
import signal
import socket
import subprocess
import time


def text(path):
    return Path(path).read_text(errors="replace").strip()


def command(argv, timeout=4):
    result = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"{argv[0]} exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout.strip()


def session(args):
    prefix = ["systemctl"]
    if args.scope == "user":
        prefix += ["--user"]
        if os.geteuid() == 0 and args.user != "root":
            uid = pwd.getpwnam(args.user).pw_uid
            prefix = ["runuser", "-u", args.user, "--", "env",
                      f"XDG_RUNTIME_DIR=/run/user/{uid}", *prefix]
    properties = command([*prefix, "show", args.unit, "--no-pager",
                          "--property=MainPID,ControlGroup,ActiveState,SubState"])
    result = dict(line.split("=", 1) for line in properties.splitlines() if "=" in line)
    if result.get("ActiveState") != "active" or not int(result.get("MainPID", "0")):
        raise RuntimeError(f"Session is not active: {result}")
    # PAMName=login can move the supervisor/hosts to a logind session scope;
    # the service's ControlGroup and CPUUsageNSec can then be almost empty.
    service_group = result.get("ControlGroup", "")
    main_pid = int(result["MainPID"])
    membership = text(f"/proc/{main_pid}/cgroup").splitlines()
    groups = [line.split(":", 2)[2] for line in membership if line.startswith("0::")]
    if len(groups) != 1:
        raise RuntimeError(f"Cannot find the MainPID's unified cgroup: {membership}")
    group = groups[0]
    if not group.startswith("/") or group == "/" or ".." in Path(group).parts:
        raise RuntimeError(f"Invalid session cgroup: {group!r}")
    root = Path("/sys/fs/cgroup") / group.lstrip("/")
    if not (root / "cgroup.procs").exists():
        raise RuntimeError(f"A unified cgroup v2 session is required: {root}")
    if main_pid not in pids_in(root):
        raise RuntimeError(f"MainPID {main_pid} is absent from its actual cgroup {root}")
    result.update(unit=args.unit, scope=args.scope, service_control_group=service_group,
                  actual_control_group=group, cgroup_path=str(root), main_pid_verified=True)
    return result, root


def pids_in(group):
    pids = set()
    for path in [group / "cgroup.procs", *group.rglob("cgroup.procs")]:
        try:
            pids.update(int(line) for line in text(path).splitlines())
        except FileNotFoundError:
            pass  # A transient descendant can disappear between enumeration/read.
    return sorted(pids)


def find_ipc(args, pids, main_pid):
    if args.ipc:
        return args.ipc
    candidates = []
    for pid in [main_pid, *pids]:
        try:
            env = dict(part.split(b"=", 1) for part in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0") if b"=" in part)
            if b"PRISMSOCK" in env:
                candidates.append(os.fsdecode(env[b"PRISMSOCK"]))
            if b"XDG_RUNTIME_DIR" in env:
                candidates.append(os.fsdecode(env[b"XDG_RUNTIME_DIR"]) + "/prism-ipc.sock")
        except (FileNotFoundError, PermissionError):
            continue
    candidates += [os.environ.get("PRISMSOCK", ""),
                   f"/run/user/{pwd.getpwnam(args.user).pw_uid}/prism-ipc.sock"]
    for candidate in candidates:
        if candidate and Path(candidate).is_socket():
            return candidate
    raise RuntimeError("Cannot find the WM IPC socket; provide --ipc PATH")


def wm_status(path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as peer:
        peer.settimeout(2)
        peer.connect(path)
        peer.sendall(b"get_status\n")
        data = bytearray()
        while True:
            part = peer.recv(65536)
            if not part:
                break
            data.extend(part)
            if len(data) > 2 * 1024 * 1024:
                raise RuntimeError("WM status exceeds capture limit")
    status = json.loads(data)
    if status.get("status") != "ok":
        raise RuntimeError(f"WM rejected get_status: {status}")
    return status


def cpu_system():
    fields = text("/proc/stat").splitlines()[0].split()
    ticks = list(map(int, fields[1:9]))  # Guest times are already counted in user/nice.
    return {"total_ticks": sum(ticks), "idle_ticks": ticks[3] + ticks[4]}


def process(pid):
    base = Path(f"/proc/{pid}")
    stat = text(base / "stat")
    end = stat.rfind(")")
    values = stat[end + 2:].split()  # comm may contain spaces or parentheses.
    result = {"pid": pid, "comm": stat[stat.find("(") + 1:end],
              "start_ticks": int(values[19]), "cpu_ticks": int(values[11]) + int(values[12]),
              "rss_kib": int(values[21]) * os.sysconf("SC_PAGE_SIZE") / 1024}
    try:
        result["cmdline"] = (base / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace").strip()
        smaps = text(base / "smaps_rollup")
        for key in ("Pss", "Rss"):
            match = re.search(rf"^{key}:\s+(\d+)\s+kB$", smaps, re.M)
            if match:
                result[key.lower() + "_kib"] = int(match[1])
    except (FileNotFoundError, ProcessLookupError, PermissionError) as error:
        result["memory_error"] = str(error)
    return result


def drm_clients(pids):
    clients, errors = {}, []
    for pid in pids:
        try:
            infos = list(Path(f"/proc/{pid}/fdinfo").iterdir())
        except (FileNotFoundError, PermissionError) as error:
            errors.append({"pid": pid, "error": str(error)})
            continue
        for path in infos:
            try:
                fields = dict(line.split(":", 1) for line in text(path).splitlines() if ":" in line)
                fields = {key: value.strip() for key, value in fields.items()}
            except FileNotFoundError:
                continue
            except PermissionError as error:
                errors.append({"pid": pid, "fd": path.name, "error": str(error)})
                continue
            driver, client_id = fields.get("drm-driver"), fields.get("drm-client-id")
            if not driver or client_id is None:
                continue
            # IDs are unique within a device, not across all devices of a driver.
            device = fields.get("drm-pdev")
            if not device:
                try:
                    stat = Path(f"/proc/{pid}/fd/{path.name}").stat()
                    identity = f"{os.major(stat.st_rdev)}:{os.minor(stat.st_rdev)}"
                    sys_device = Path(f"/sys/dev/char/{identity}/device")
                    device = str(sys_device.resolve()) if sys_device.exists() else f"char:{identity}"
                except FileNotFoundError:
                    continue
                except PermissionError as error:
                    errors.append({"pid": pid, "fd": path.name, "error": str(error)})
                    continue
            key = (driver, device, client_id)
            client = clients.setdefault(key, {"driver": driver, "device": device, "client_id": client_id,
                "references": [], "engine_ns": {}, "resident_kib": {}, "memory_kib": {}, "raw": {}})
            client["references"].append({"pid": pid, "fd": int(path.name)})
            client["raw"].update({k: v for k, v in fields.items() if k.startswith("drm-")})
            for name, value in fields.items():
                match = re.fullmatch(r"(\d+)\s*(ns|KiB|kB|MiB|B)?", value)
                if not match:
                    continue
                amount, unit = int(match[1]), match[2]
                if name.startswith("drm-engine-") and unit == "ns":
                    metric, bucket = name.removeprefix("drm-engine-"), "engine_ns"
                elif name.startswith(("drm-resident-", "drm-memory-")) and unit in (None, "KiB", "kB", "MiB", "B"):
                    metric = name.removeprefix("drm-resident-").removeprefix("drm-memory-")
                    bucket = "resident_kib"
                    amount *= {"KiB": 1, "kB": 1, "MiB": 1024, "B": 1 / 1024, None: 1 / 1024}[unit]
                elif name.startswith(("drm-total-", "drm-shared-", "drm-purgeable-", "drm-active-")) and unit in (None, "KiB", "kB", "MiB", "B"):
                    metric, bucket = name, "memory_kib"
                    amount *= {"KiB": 1, "kB": 1, "MiB": 1024, "B": 1 / 1024, None: 1 / 1024}[unit]
                else:
                    continue
                # The same DRM client can be open in several FDs/processes.
                # Their counters are views of one client, never additive.
                client[bucket][metric] = max(amount, client[bucket].get(metric, 0))
    return sorted(clients.values(), key=drm_key), errors


def drm_key(client):
    return client["driver"], client["device"], client["client_id"]


def hardware():
    result = {"thermal": [], "cpufreq": [], "errors": []}
    for directory in sorted(Path("/sys/class/thermal").glob("thermal_zone*")):
        try:
            result["thermal"].append({"zone": directory.name, "type": text(directory / "type"),
                                      "temperature_c": int(text(directory / "temp")) / 1000})
        except OSError as error:
            result["errors"].append(str(error))
    for directory in sorted(Path("/sys/devices/system/cpu/cpufreq").glob("policy*")):
        policy = {"policy": directory.name}
        for name in ("scaling_cur_freq", "scaling_min_freq", "scaling_max_freq", "scaling_governor"):
            try:
                value = text(directory / name)
                policy[name + ("_khz" if name.endswith("freq") else "")] = int(value) if value.isdigit() else value
            except OSError as error:
                result["errors"].append(str(error))
        result["cpufreq"].append(policy)
    executable = shutil.which("vcgencmd")
    if executable:
        try:
            raw = command([executable, "get_throttled"])
            match = re.search(r"0x[0-9a-fA-F]+", raw)
            mask = int(match[0], 16) if match else None
            names = {0: "under_voltage", 1: "frequency_capped", 2: "throttled", 3: "soft_temperature_limit",
                     16: "under_voltage_since_boot", 17: "frequency_capped_since_boot",
                     18: "throttled_since_boot", 19: "soft_temperature_limit_since_boot"}
            result["vcgencmd"] = {"raw": raw, "mask": mask,
                "flags": [name for bit, name in names.items() if mask is not None and mask & (1 << bit)]}
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result["errors"].append(str(error))
    return result


def capture(group, ipc, drm_highwater):
    start = time.monotonic_ns()
    pids, processes, errors = pids_in(group), [], []
    try:
        status = wm_status(ipc)
    except (OSError, ValueError, RuntimeError) as error:
        status = {"capture_error": str(error)}
    for pid in pids:
        try:
            processes.append(process(pid))
        except (OSError, ValueError, IndexError) as error:
            errors.append({"pid": pid, "error": str(error)})
    drm, drm_errors = drm_clients(pids)
    for client in drm:
        client["engine_highwater_ns"] = {}
        client["engine_counter_regressions"] = []
        for engine, ns in client["engine_ns"].items():
            key = (*drm_key(client), engine)
            previous = drm_highwater.get(key, ns)
            if ns < previous:
                client["engine_counter_regressions"].append(engine)
            drm_highwater[key] = max(previous, ns)
            client["engine_highwater_ns"][engine] = drm_highwater[key]
    try:
        group_cpu = dict(line.split() for line in text(group / "cpu.stat").splitlines())
        group_cpu = {key: int(value) for key, value in group_cpu.items()}
    except OSError as error:
        group_cpu = {"capture_error": str(error)}
    return {"monotonic_ns": start, "collection_ms": (time.monotonic_ns() - start) / 1e6,
            "wm": status, "pids": pids, "processes": processes, "process_errors": errors,
            "drm_clients": drm, "drm_errors": drm_errors, "cgroup_cpu": group_cpu,
            "system_cpu": cpu_system()}


def deltas(before, after, clock_ticks):
    elapsed = (after["monotonic_ns"] - before["monotonic_ns"]) / 1e9
    old = {(p["pid"], p["start_ticks"]): p for p in before["processes"]}
    rows = []
    for proc in after["processes"]:
        previous = old.get((proc["pid"], proc["start_ticks"]))
        if previous:
            ticks = proc["cpu_ticks"] - previous["cpu_ticks"]
            rows.append({"pid": proc["pid"], "start_ticks": proc["start_ticks"], "comm": proc["comm"],
                         "delta_ticks": ticks, "cpu_percent_one_core": ticks / clock_ticks / elapsed * 100})
    result = {"elapsed_seconds": elapsed, "processes": rows,
              "stable_process_cpu_percent_one_core": sum(p["cpu_percent_one_core"] for p in rows)}
    first, last = before["cgroup_cpu"], after["cgroup_cpu"]
    if "usage_usec" in first and "usage_usec" in last:
        result["cgroup_cpu_percent_one_core"] = (last["usage_usec"] - first["usage_usec"]) / 1e6 / elapsed * 100
    total = after["system_cpu"]["total_ticks"] - before["system_cpu"]["total_ticks"]
    idle = after["system_cpu"]["idle_ticks"] - before["system_cpu"]["idle_ticks"]
    result["whole_machine_busy_percent"] = (total - idle) / total * 100 if total else None
    old_drm = {drm_key(c): c for c in before["drm_clients"]}
    engine_rows = []
    for client in after["drm_clients"]:
        prior = old_drm.get(drm_key(client))
        if not prior:
            continue
        for engine, ns in client["engine_highwater_ns"].items():
            if engine in prior["engine_highwater_ns"]:
                delta = ns - prior["engine_highwater_ns"][engine]
                engine_rows.append({"driver": client["driver"], "device": client["device"],
                                    "client_id": client["client_id"], "engine": engine,
                                    "delta_ns": delta, "counter_regressed": engine in client["engine_counter_regressions"],
                                    "busy_percent": delta / (elapsed * 1e9) * 100 if delta >= 0 else None})
    result["drm_engine_busy"] = engine_rows
    return result


def aggregate(samples, ticks, cpu_count):
    first, last = samples[0], samples[-1]
    intervals = [deltas(a, b, ticks) for a, b in zip(samples, samples[1:])]
    memory = [{"monotonic_ns": sample["monotonic_ns"],
               "pss_kib": sum(p.get("pss_kib", 0) for p in sample["processes"]),
               "rss_kib": sum(p["rss_kib"] for p in sample["processes"]),
               "pss_processes": sum("pss_kib" in p for p in sample["processes"]),
               "total_processes": len(sample["processes"])} for sample in samples]
    result = {"intervals": intervals, "memory": memory,
              "cpu_units": {"one_core_percent": 100, "machine_capacity_percent": cpu_count * 100,
                            "online_cpus": cpu_count},
              "notes": ["RSS totals count shared mappings more than once; PSS is reported separately.",
                        "DRM resident allocations are deduplicated by driver/device/client-id, never added to PSS/RSS.",
                        "Different DRM clients may still reference shared buffers; resident classes are not a unique physical total.",
                        "DRM engine percentages are per-client busy counter deltas; concurrent engines/clients need not sum to 100%.",
                        "Regressing DRM engine counters retain the previous high-water value until catch-up, without double-accounting recovery.",
                        "Stable-process CPU excludes processes that disappear between samples; cgroup CPU includes them.",
                        "WM timings are rolling windows of up to sample_capacity samples; overlapping percentile windows are not merged.",
                        "Pointer event age is receipt minus kernel timestamp, not input-to-photon latency."]}
    if len(samples) > 1:
        result["run_delta"] = deltas(first, last, ticks)
        value = result["run_delta"].get("cgroup_cpu_percent_one_core")
        result["session_machine_capacity_percent"] = value / cpu_count if value is not None else None
    result["pss_peak_kib"] = max(m["pss_kib"] for m in memory)
    result["rss_peak_kib"] = max(m["rss_kib"] for m in memory)
    result["drm_end"] = last["drm_clients"]
    resident_samples, resident_peaks = [], {}
    for sample in samples:
        classes = {}
        for client in sample["drm_clients"]:
            for region, kib in client["resident_kib"].items():
                key = (client["driver"], client["device"], region)
                classes[key] = classes.get(key, 0) + kib
        resident_samples.append({"monotonic_ns": sample["monotonic_ns"], "classes": [
            {"driver": key[0], "device": key[1], "region": key[2], "kib": kib} for key, kib in classes.items()]})
        for key, kib in classes.items():
            resident_peaks[key] = max(kib, resident_peaks.get(key, 0))
    result["drm_resident_samples"] = resident_samples
    result["drm_resident_class_peaks"] = [{"driver": key[0], "device": key[1], "region": key[2], "kib": kib}
                                           for key, kib in resident_peaks.items()]
    result["wm_end_rolling"] = last["wm"].get("performance")
    before, after = first["wm"].get("performance", {}), last["wm"].get("performance", {})
    result["wm_counter_delta"] = {key: after[key] - before[key] for key in
        ("commit_successes", "commit_failures", "pointer_events") if key in before and key in after}
    old_outputs = {row["output"]: row for row in before.get("presentation", [])}
    result["presentation_delta"] = [{"output": row["output"], **{key: row[key] - old_outputs[row["output"]][key]
        for key in ("presented", "discarded")}} for row in after.get("presentation", []) if row["output"] in old_outputs]
    for name in ("frame_cpu", "effects_cpu", "commit_cpu", "pointer_event_age"):
        windows = [s["wm"].get("performance", {}).get(name, {}) for s in samples]
        observed = [w["p95_ms"] for w in windows if w.get("samples", 0) and "p95_ms" in w]
        if observed:
            result.setdefault("max_observed_rolling_p95_ms", {})[name] = max(observed)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", default="idle")
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--user", default=os.environ.get("SUDO_USER") or getpass.getuser())
    parser.add_argument("--scope", choices=("system", "user"), default="system")
    parser.add_argument("--unit", help="default: prism-demo@USER.service (system), prism-session.service (user)")
    parser.add_argument("--ipc", help="WM Unix socket override")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error("--seconds must be between 1 and 3600")
    if not args.unit:
        args.unit = f"prism-demo@{args.user}.service" if args.scope == "system" else "prism-session.service"
    args.output.mkdir(parents=True, exist_ok=True)
    report_file = args.output / "performance-report.json"
    if report_file.exists():
        parser.error(f"refusing to overwrite {report_file}")
    metadata, group = session(args)
    ipc = find_ipc(args, pids_in(group), int(metadata["MainPID"]))
    stopped = []
    def stop(signum, _frame):
        stopped.append(signum)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    report = {"schema_version": 1, "label": args.label, "requested_seconds": args.seconds,
              "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "session": metadata,
              "ipc": ipc, "observer_pid": os.getpid(), "root": os.geteuid() == 0,
              "clock_ticks_per_second": os.sysconf("SC_CLK_TCK"), "hardware_start": hardware(), "samples": []}
    try:
        with (args.output / "samples.jsonl").open("x") as raw:
            drm_highwater = {}
            origin = time.monotonic()
            next_sample, deadline = origin, origin + args.seconds
            while True:
                sample = capture(group, ipc, drm_highwater)
                sample["contains_main_pid"] = int(metadata["MainPID"]) in sample["pids"]
                report["samples"].append(sample)
                raw.write(json.dumps(sample, separators=(",", ":")) + "\n")
                raw.flush()
                if stopped or time.monotonic() >= deadline:
                    break
                next_sample += 1
                while next_sample <= time.monotonic():
                    next_sample += 1
                target = min(next_sample, deadline)
                while not stopped and time.monotonic() < target:
                    time.sleep(min(.1, target - time.monotonic()))
    finally:
        report["hardware_end"] = hardware()
        report["termination_signals"] = stopped
        if report["samples"]:
            report["summary"] = aggregate(report["samples"], report["clock_ticks_per_second"], os.cpu_count() or 1)
        start_mask = report["hardware_start"].get("vcgencmd", {}).get("mask")
        end_mask = report["hardware_end"].get("vcgencmd", {}).get("mask")
        if start_mask is not None and end_mask is not None:
            report["throttle_new_bits"] = end_mask & ~start_mask
        report_file.write_text(json.dumps(report, indent=2) + "\n")
    summary = report.get("summary", {})
    print(json.dumps({"report": str(report_file), "label": args.label,
                      "samples": len(report["samples"]),
                      "session_cpu_percent_one_core": summary.get("run_delta", {}).get("cgroup_cpu_percent_one_core"),
                      "pss_peak_kib": summary.get("pss_peak_kib")}, separators=(",", ":")))
    return 0 if not any("capture_error" in s["wm"] or not s["contains_main_pid"] for s in report["samples"]) else 1


if __name__ == "__main__":
    raise SystemExit(main())
