"""Regression for self-reported failure followed by unresponsive cleanup."""
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
root=Path(__file__).resolve().parents[2]
build=Path(sys.argv[1] if len(sys.argv)>1 else root/'build-gles').resolve()
with tempfile.TemporaryDirectory(prefix='prism-failed-worker-') as directory:
    env=dict(os.environ,XDG_RUNTIME_DIR=directory)
    with open(Path(directory)/'launcher.log','w+') as log:
        service=subprocess.Popen([str(build/'bin/prism-launcher'),'--apps-root',str(build/'share/prism/apps'),
            '--host',str(root/'tests/fixtures/failed_worker.py'),'--pool-size','0','--startup-timeout-ms','60000'],env=env,stdout=log,stderr=log)
        peer=socket.socket(socket.AF_UNIX); peer.settimeout(5)
        try:
            path=Path(directory)/'prism/launcher.sock'; deadline=time.monotonic()+5
            while not path.exists():
                assert service.poll() is None
                assert time.monotonic()<deadline
                time.sleep(.02)
            peer.connect(str(path)); name=b'demo_player'; body=struct.pack('>BH',1,len(name))+name
            peer.sendall(struct.pack('>IHHIQQ',0x50524c31,1,1,len(body),1,0)+body)
            def exact(n):
                b=b''
                while len(b)<n:
                    part=peer.recv(n-len(b)); assert part; b+=part
                return b
            seen=[]
            while True:
                magic,version,kind,size,request,instance=struct.unpack('>IHHIQQ',exact(28))
                assert (magic,version,kind,request)==(0x50524c31,1,2,1)
                pid,milestone,error,code,length=struct.unpack('>IBHiH',exact(size)[:13]); seen.append((milestone,error,code))
                if milestone==7:
                    assert seen==[(0,0,0),(1,0,0),(2,0,0),(6,7,0),(7,0,-9)],seen
                    assert not Path(f'/proc/{pid}').exists()
                    break
            print('Worker self failure retained; blocked cleanup was killed/reaped within 5 seconds, before 60-second startup watchdog.')
        finally:
            peer.close(); service.terminate()
            try: service.wait(timeout=5)
            except subprocess.TimeoutExpired: service.kill(); service.wait()
