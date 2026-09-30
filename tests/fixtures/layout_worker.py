#!/usr/bin/env python3
"""Launcher fixture: real worker endpoint, without a Wayland frontend."""
import json
import os
from pathlib import Path
import socket
import struct
import sys
import time

channel = socket.socket(fileno=int(sys.argv[sys.argv.index('--worker-fd') + 1]))
channel.settimeout(10)

def send(kind, body):
    channel.sendall(struct.pack('>IHHI', 0x50525731, 1, kind, len(body)) + body)

def exact(size):
    result = b''
    while len(result) < size:
        part = channel.recv(size - len(result))
        if not part:
            raise SystemExit(1)
        result += part
    return result

def receive():
    magic, version, kind, size = struct.unpack('>IHHI', exact(12))
    assert magic == 0x50525731 and version == 1 and size <= 192 * 1024 + 11
    return kind, exact(size)

send(1, struct.pack('>Q', 1))
while True:
    kind, body = receive()
    if kind == 9:
        generation = struct.unpack_from('>Q', body, 4)[0]
        send(10, struct.pack('>QBH', generation, 1, 0))
    else:
        assert kind == 2
        break

instance = struct.unpack_from('>Q', body)[0]
request = struct.unpack_from('>Q', body, 20)[0]
app_size = struct.unpack_from('>H', body, 37)[0]
app = body[39:39 + app_size].decode()
subscription = 77
send(13, struct.pack('>HQB', 1, subscription, 1))
# Begin: independent request namespace; fixed DTO contains no role/instance/PID.
payload = struct.pack('>HQQQQBBB7QB Ii dd', 1, 101, 201, 0, 1, 0, 0, 0,
                      55, 1, 1, 1, 0, 1, 1, 0, 33, 0, 40., 10.)
assert len(payload) == 118
send(15, payload)
seen = {}
while len(seen) < 2:
    kind, body = receive()
    if kind == 14:
        version, identifier, status = struct.unpack_from('>HQB', body)
        assert version == 1 and identifier == subscription
        assert status == (0 if app in ('prism_topbar', 'prism_dock') else 1)
        seen['subscription'] = status
    elif kind == 16:
        version, identifier, gesture, session, sequence, status, error = struct.unpack_from(
            '>HQQQQBB', body)
        assert (version, identifier, gesture, sequence) == (1, 101, 201, 1)
        if app == 'prism_topbar':
            assert (session, status, error) == (900, 0, 0)
        else:
            assert (session, status, error) == (0, 4, 1)
        seen['control'] = [session, status, error]
    else:
        raise AssertionError((app, kind))

Path(os.environ['PRISM_LAYOUT_ROUTING_RESULTS'], app + '.json').write_text(json.dumps(seen))
# Stay alive while the parent checks that no replies leaked between workers.
while True:
    time.sleep(10)
