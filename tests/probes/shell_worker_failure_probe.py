"""Shell self-failure, bounded cleanup and late WM replies; no frontend or installation."""
import os
from pathlib import Path
import select
import socket
import struct
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv) > 1 else root / 'build-gles').resolve()


def exact(peer, count):
    data = b''
    while len(data) < count:
        part = peer.recv(count - len(data))
        assert part
        data += part
    return data


def control(kind, body):
    return struct.pack('>IHHI', 0x50574331, 1, kind, len(body)) + body


def permit():
    return struct.pack('>QQQIB32sQB', 55, 0, 0, 0, 0, b'\0' * 32, 0, 1)


with tempfile.TemporaryDirectory(prefix='prism-shell-failure-') as directory:
    runtime = Path(directory)
    wm, inherited = socket.socketpair()
    wm.settimeout(2)
    env = dict(os.environ, XDG_RUNTIME_DIR=directory)
    log_path = runtime / 'launcher.log'
    with log_path.open('w+') as log:
        command = [str(build / 'bin/prism-launcher'), '--apps-root', str(build / 'share/prism/apps'),
                   '--host', str(root / 'tests/fixtures/failed_worker.py'), '--pool-size', '0',
                   '--startup-timeout-ms', '60000', '--wm-fd', str(inherited.fileno()),
                   '--parent-pid', str(os.getpid()), '--start-shell']
        service = subprocess.Popen(command, env=env, pass_fds=(inherited.fileno(),),
                                   stdout=log, stderr=log)
        inherited.close()
        public = None
        grants = {}
        revoked = set()
        try:
            wm.sendall(control(1, permit()))
            deadline = time.monotonic() + 10
            ordinary_sent = False
            late_sent = False
            completed = False
            events = []
            while time.monotonic() < deadline:
                assert service.poll() is None, log_path.read_text()
                if public is None and (runtime / 'prism/launcher.sock').exists():
                    public = socket.socket(socket.AF_UNIX)
                    public.settimeout(2)
                    public.connect(str(runtime / 'prism/launcher.sock'))

                shell = {pid: body for pid, body in grants.items() if body[28] != 0}
                if (len(shell) == 4 and not late_sent and
                        all(not Path(f'/proc/{pid}').exists() for pid in shell)):
                    assert set(shell).issubset(revoked)
                    assert log_path.read_text().count('policy=keep-session restart=disabled') == 4
                    # SIGCHLD can beat the final WM frame. Retired identities
                    # must validate these messages without reviving a worker.
                    for body in shell.values():
                        wm.sendall(control(3, body) + control(7, body) + control(8, body))
                    late_sent = True

                if late_sent and not ordinary_sent:
                    name = b'demo_player'
                    body = struct.pack('>BH', 1, len(name)) + name
                    public.sendall(struct.pack('>IHHIQQ', 0x50524c31, 1, 1, len(body), 1, 0) + body)
                    ordinary_sent = True

                peers = [wm] + ([public] if public is not None else [])
                ready, _, _ = select.select(peers, [], [], .02)
                for peer in ready:
                    if peer is public:
                        magic, version, kind, size, request, instance = struct.unpack(
                            '>IHHIQQ', exact(public, 28))
                        assert (magic, version, kind, request) == (0x50524c31, 1, 2, 1)
                        pid, milestone, error, code, length = struct.unpack(
                            '>IBHiH', exact(public, size)[:13])
                        events.append((milestone, error, code))
                        if milestone == 7:
                            assert events == [(0, 0, 0), (1, 0, 0), (2, 0, 0),
                                              (6, 7, 0), (7, 0, -9)], events
                            assert not Path(f'/proc/{pid}').exists()
                            completed = True
                        continue

                    magic, version, kind, size = struct.unpack('>IHHI', exact(wm, 12))
                    assert (magic, version) == (0x50574331, 1)
                    body = exact(wm, size)
                    if kind == 11:
                        assert body == struct.pack('>QB', 55, 1)
                    elif kind == 9:
                        generation = struct.unpack_from('>Q', body, 12)[0]
                        wm.sendall(control(10, struct.pack('>QQBH', 55, generation, 1, 0)))
                    elif kind == 2:
                        pid = struct.unpack_from('>I', body, 24)[0]
                        reply = body[:-1] + b'\1'
                        grants[pid] = reply
                        wm.sendall(control(3, reply))
                    elif kind == 4:
                        revoked.add(struct.unpack_from('>I', body, 24)[0])
                    else:
                        raise AssertionError(kind)
                if completed:
                    break

            assert completed and len(grants) == 5, (events, log_path.read_text())
            assert service.poll() is None, log_path.read_text()
            service.terminate()
            assert service.wait(timeout=5) == 0, log_path.read_text()
            print('Four Shell self-failures were revoked and reaped; late WM replies and a new ordinary launch preserved the session.')
        except Exception:
            print(log_path.read_text(), file=sys.stderr)
            raise
        finally:
            if public is not None:
                public.close()
            if service.poll() is None:
                service.terminate()
                try:
                    service.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    service.kill()
                    service.wait()
            wm.close()
