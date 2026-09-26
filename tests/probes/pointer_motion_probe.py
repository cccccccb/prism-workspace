#!/usr/bin/env python3
"""Generate bounded, click-free relative pointer motion through kernel uinput.

Run with permission to open /dev/uinput. Device discovery plus --settle occurs
before the measured motion; ready.json marks the beginning if --output is set.
Pointer acceleration and screen-edge clipping mean final restoration is approximate.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import signal
import struct
import time

EV_SYN, EV_KEY, EV_REL = 0, 1, 2
REL_X, REL_Y, SYN_REPORT = 0, 1, 0
BTN_LEFT, BTN_RIGHT, BUS_USB = 0x110, 0x111, 3
INPUT_PROP_POINTER = 0


def ioctl_code(direction, number, size=0):
    return (direction << 30) | (size << 16) | (ord("U") << 8) | number


UI_DEV_CREATE, UI_DEV_DESTROY = ioctl_code(0, 1), ioctl_code(0, 2)
UI_DEV_SETUP = ioctl_code(1, 3, 92)
UI_SET_EVBIT, UI_SET_KEYBIT, UI_SET_RELBIT = (ioctl_code(1, number, 4) for number in (100, 101, 102))
UI_SET_PROPBIT = ioctl_code(1, 110, 4)
EVENT = struct.Struct("@llHHi")


def emit(fd, dx, dy):
    data = EVENT.pack(0, 0, EV_REL, REL_X, dx) + EVENT.pack(0, 0, EV_REL, REL_Y, dy) + EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0)
    while data:
        amount = os.write(fd, data)
        if not amount:
            raise OSError("uinput write returned zero")
        data = data[amount:]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--seconds", type=float, default=12)
    parser.add_argument("--hz", type=float, default=125)
    parser.add_argument("--excursion", type=int, default=48, help="maximum excursion in relative input units per axis")
    parser.add_argument("--settle", type=float, default=2, help="seconds for libinput to discover the device")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or not 1 <= args.hz <= 1000 or not 1 <= args.excursion <= 128 or not 1 <= args.settle <= 10:
        parser.error("seconds 1..3600, hz 1..1000, excursion 1..128, settle 1..10 are required")
    if args.output:
        args.output.mkdir(parents=True, exist_ok=True)
        if (args.output / "pointer-report.json").exists() or (args.output / "ready.json").exists():
            parser.error("output already contains a pointer run")
    signals = []
    def stop(signum, _frame):
        signals.append(signum)
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)
    report = {"hz": args.hz, "requested_seconds": args.seconds, "excursion": args.excursion,
              "input_event_bytes": EVENT.size, "clicks_sent": 0, "motion_reports": 0, "cleanup_errors": []}
    fd, created, offset = None, False, 0
    try:
        fd = os.open("/dev/uinput", os.O_WRONLY | os.O_CLOEXEC)
        for kind in (EV_SYN, EV_KEY, EV_REL):
            fcntl.ioctl(fd, UI_SET_EVBIT, kind)
        # Mouse capability makes libinput recognize it; no EV_KEY is emitted.
        for button in (BTN_LEFT, BTN_RIGHT):
            fcntl.ioctl(fd, UI_SET_KEYBIT, button)
        for axis in (REL_X, REL_Y):
            fcntl.ioctl(fd, UI_SET_RELBIT, axis)
        fcntl.ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER)
        setup = struct.pack("@HHHH80sI", BUS_USB, 0x1209, 0x5072, 1,
                            b"Prism independent pointer probe", 0)
        fcntl.ioctl(fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        created = True
        name = bytearray(128)
        fcntl.ioctl(fd, ioctl_code(2, 44, len(name)), name, True)
        sysname = bytes(name).split(b"\0", 1)[0].decode()
        report["sysname"] = sysname
        discovery_deadline = time.monotonic() + 5
        event_node = None
        while time.monotonic() < discovery_deadline and not signals:
            candidates = list((Path("/sys/devices/virtual/input") / sysname).glob("event*"))
            if candidates:
                node = Path("/dev/input") / candidates[0].name
                if node.exists():
                    event_node = node
                    break
            time.sleep(.05)
        if not event_node and not signals:
            raise RuntimeError("uinput event node did not appear")
        if signals:
            return 130
        report["event_node"] = str(event_node)
        settled = time.monotonic() + args.settle
        while time.monotonic() < settled and not signals:
            time.sleep(.05)
        if signals:
            return 130
        start = time.monotonic()
        ready = {"event": "ready", "sysname": sysname, "event_node": str(event_node),
                 "monotonic_ns": time.monotonic_ns(), "hz": args.hz}
        if args.output:
            (args.output / "ready.json").write_text(json.dumps(ready) + "\n")
        print(json.dumps(ready), flush=True)
        direction, period, scheduled = 1, 1 / args.hz, start
        lateness_sum, lateness_max = 0, 0
        while time.monotonic() - start < args.seconds and not signals:
            now = time.monotonic()
            if now < scheduled:
                time.sleep(scheduled - now)
            if signals:
                break
            now = time.monotonic()
            lateness = max(0, now - scheduled) * 1000
            lateness_sum += lateness
            lateness_max = max(lateness_max, lateness)
            emit(fd, direction, direction)
            offset += direction
            report["motion_reports"] += 1
            if offset >= args.excursion:
                direction = -1
            elif offset <= 0:
                direction = 1
            scheduled += period
            if scheduled < now - period:
                scheduled = now + period  # Skip missed slots instead of a burst.
        report["elapsed_seconds"] = time.monotonic() - start
        report["actual_hz"] = report["motion_reports"] / report["elapsed_seconds"]
        report["scheduler_lateness_max_ms"] = lateness_max
        report["scheduler_lateness_mean_ms"] = lateness_sum / report["motion_reports"] if report["motion_reports"] else 0
    finally:
        # Compensate the net relative displacement without any clicks. Normal
        # pointer acceleration/edge clipping can prevent exact pixel restoration.
        if fd is not None:
            if created:
                restored = 0
                try:
                    while offset:
                        direction = -1 if offset > 0 else 1
                        emit(fd, direction, direction)
                        offset += direction
                        restored += 1
                        time.sleep(1 / args.hz)
                    report["restoration_reports"] = restored
                except OSError as error:
                    report["cleanup_errors"].append(str(error))
                try:
                    fcntl.ioctl(fd, UI_DEV_DESTROY)
                except OSError as error:
                    report["cleanup_errors"].append(str(error))
            os.close(fd)
        report["termination_signals"] = signals
        report["remaining_relative_offset"] = offset
        if args.output:
            (args.output / "pointer-report.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report), flush=True)
    return 130 if signals else 0


if __name__ == "__main__":
    raise SystemExit(main())
