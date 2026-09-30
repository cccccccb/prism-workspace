"""Exercise worker→launcher→WM identities using real launcher/worker processes."""
import json
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

def control(kind, body):
    return struct.pack('>IHHI', 0x50574331, 1, kind, len(body)) + body

def permit(request=0, instance=0, pid=0, role=0, success=1):
    return struct.pack('>QQQIB32sQB', 55, request, instance, pid, role, b'\0' * 32, 0, success)

def exact(peer, size):
    result = b''
    while len(result) < size:
        part = peer.recv(size - len(result))
        assert part
        result += part
    return result

with tempfile.TemporaryDirectory(prefix='prism-layout-routing-') as directory:
    path = Path(directory)
    env = dict(os.environ, XDG_RUNTIME_DIR=directory, PRISM_LAYOUT_ROUTING_RESULTS=directory)
    wm, child = socket.socketpair()
    wm.settimeout(5)
    log = open(path / 'launcher.log', 'w+')
    command = [str(build / 'bin/prism-launcher'), '--apps-root', str(build / 'share/prism/apps'),
               '--host', str(root / 'tests/fixtures/layout_worker.py'), '--pool-size', '0',
               '--startup-timeout-ms', '60000', '--wm-fd', str(child.fileno()),
               '--parent-pid', str(os.getpid()), '--start-shell']
    service = subprocess.Popen(command, env=env, pass_fds=(child.fileno(),), stdout=log, stderr=log)
    child.close()
    client = None
    grants = {}
    routed = 0
    try:
        wm.sendall(control(1, permit()))
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            assert service.poll() is None
            endpoint = path / 'prism/launcher.sock'
            if client is None and endpoint.exists():
                client = socket.socket(socket.AF_UNIX)
                client.connect(str(endpoint))
                name = b'demo_player'
                body = struct.pack('>BH', 1, len(name)) + name
                client.sendall(struct.pack('>IHHIQQ', 0x50524c31, 1, 1, len(body), 1, 0) + body)
            if len(list(path.glob('*.json'))) == 4:
                break
            ready, _, _ = select.select([wm], [], [], .02)
            if not ready:
                continue
            magic, version, kind, size = struct.unpack('>IHHI', exact(wm, 12))
            assert (magic, version) == (0x50574331, 1)
            body = exact(wm, size)
            if kind == 11:
                assert body == struct.pack('>QB', 55, 1)
                snapshot = struct.pack('>H6Q4H', 1, 55, 1, 1, 1, 1, 0, 0, 0, 0, 0)
                wm.sendall(control(12, struct.pack('>Q', 55) + snapshot))
            elif kind == 9:
                generation = struct.unpack_from('>Q', body, 12)[0]
                wm.sendall(control(10, struct.pack('>QQBH', 55, generation, 1, 0)))
            elif kind == 2:
                session, request, instance, pid, role = struct.unpack_from('>QQQIB', body)
                assert session == 55 and instance and pid
                grants[pid] = (request, instance, role)
                reply = bytearray(body)
                reply[-1] = 1
                wm.sendall(control(3, reply) + control(7, reply))
            elif kind == 13:
                session, request, instance, pid, role = struct.unpack_from('>QQQIB', body)
                assert session == 55 and grants[pid] == (request, instance, role) and role == 2
                assert len(body) == 188
                identifier, gesture, sequence = 101, 201, 1
                result = struct.pack('>HQQQQBBQQQddB', 1, identifier, gesture, 900, sequence,
                                     0, 0, 1, 1, 1, 40., 10., 0)
                # A correct PID with a wrong immutable instance must not deliver.
                wm.sendall(control(14, permit(request, instance + 99, pid, role) + result))
                wm.sendall(control(14, permit(request, instance, pid, role) + result))
                routed += 1
            elif kind == 4:
                pass
            else:
                raise AssertionError(kind)
        files = sorted(path.glob('*.json'))
        assert len(files) == 4, [file.name for file in files]
        assert routed == 1 and len(grants) == 4
        print(json.dumps({file.stem: json.loads(file.read_text()) for file in files}, indent=2))
        print('Trusted subscriptions, ordinary/Dock denial, immutable owner routing passed.')
    finally:
        if client:
            client.close()
        service.terminate()
        try:
            service.wait(timeout=5)
        except subprocess.TimeoutExpired:
            service.kill()
            service.wait()
        wm.close()
        log.seek(0)
        if service.returncode not in (0, -15):
            print(log.read())
        log.close()
