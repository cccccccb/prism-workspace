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
# Both operations use independent request IDs; the launcher must enforce roles.
for operation in (0, 1):
    payload = struct.pack('>HQQQQBBB7QB Ii dd', 1, 101 + operation, 201 + operation,
                          0, 1, 0, operation, 0, 55, 1, 1, 1,
                          17 if operation else 0, 1, 1, 0, 33, 0, 40., 10.)
    assert len(payload) == 118
    send(15, payload)
seen = {'controls': {}}
while 'subscription' not in seen or len(seen['controls']) != 2:
    kind, body = receive()
    if kind == 14:
        version, identifier, status = struct.unpack_from('>HQB', body)
        assert version == 1 and identifier == subscription
        assert status == (0 if app in ('prism_topbar', 'prism_dock',
                                      'prism_layout_controls') else 1)
        assert 'subscription' not in seen
        seen['subscription'] = status
    elif kind == 16:
        version, identifier, gesture, session, sequence, status, error = struct.unpack_from(
            '>HQQQQBB', body)
        operation = identifier - 101
        assert operation in (0, 1)
        assert (version, gesture, sequence) == (1, 201 + operation, 1)
        allowed = ((app == 'prism_topbar' and operation == 0) or
                   (app == 'prism_layout_controls' and operation == 1))
        assert (session, status, error) == ((900, 0, 0) if allowed else (0, 4, 1))
        assert operation not in seen['controls']
        seen['controls'][operation] = [session, status, error]
    else:
        raise AssertionError((app, kind))

Path(os.environ['PRISM_LAYOUT_ROUTING_RESULTS'], app + '.json').write_text(json.dumps(seen))
# Stay alive while the parent checks that no replies leaked between workers.
while True:
    time.sleep(10)
