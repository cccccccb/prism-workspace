#!/usr/bin/env python3
"""Worker fixture: valid self failure, then blocked cleanup ignoring TERM."""
import os
import signal
import socket
import struct
import sys
import time
fd=int(sys.argv[sys.argv.index('--worker-fd')+1])
channel=socket.socket(fileno=fd)
channel.setblocking(True)
signal.signal(signal.SIGTERM,signal.SIG_IGN)
def send(kind,body):
    channel.sendall(struct.pack('>IHHI',0x50525731,1,kind,len(body))+body)
def exact(n):
    b=b''
    while len(b)<n:
        part=channel.recv(n-len(b))
        if not part: raise SystemExit(1)
        b+=part
    return b
send(1,struct.pack('>Q',1))
while True:
    magic,version,kind,size=struct.unpack('>IHHI',exact(12))
    assert (magic,version)==(0x50525731,1) and size<=65536
    payload=exact(size)
    if kind==9:
        # Current production workers acknowledge the initial resolved theme
        # before they can be assigned. This fixture has no frontend to style.
        schema,generation=struct.unpack('>IQ',payload[:12])
        assert schema in (1,2) and generation
        send(10,struct.pack('>QBH',generation,1,0))
    else:
        assert kind==2
        bind=payload
        break
instance=struct.unpack('>Q',bind[:8])[0]
request=struct.unpack('>Q',bind[20:28])[0]
for milestone,error in ((2,0),(6,7)):
    body=struct.pack('>IBHiH',os.getpid(),milestone,error,0,0)
    event=struct.pack('>IHHIQQ',0x50524c31,1,2,len(body),request,instance)+body
    send(3,event)
while True: time.sleep(60)
