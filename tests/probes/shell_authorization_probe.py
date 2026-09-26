"""Trusted-parent negative grant checks; no installed test client or controller."""
import os
from pathlib import Path
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
root=Path(__file__).resolve().parents[2]
build=Path(sys.argv[1] if len(sys.argv)>1 else root/'build-gles').resolve()
with tempfile.TemporaryDirectory(prefix='prism-auth-') as directory:
    env=dict(os.environ,XDG_RUNTIME_DIR=directory,WLR_BACKENDS='headless',WLR_RENDERER='gles2',WAYLAND_DISPLAY='wayland-prism-0')
    for node in Path('/sys/class/drm').glob('renderD*'):
        if (node/'device/driver').exists() and (node/'device/driver').resolve().name=='v3d': env['WLR_RENDER_DRM_DEVICE']='/dev/dri/'+node.name
    parent,child=socket.socketpair(); parent.settimeout(5)
    log_path=Path(directory)/'wm.log'
    with log_path.open('w+') as log:
        wm=subprocess.Popen([str(build/'bin/prism-wm'),'--control-fd',str(child.fileno()),'--parent-pid',str(os.getpid())],pass_fds=(child.fileno(),),env=env,stdout=log,stderr=log)
        child.close(); client=None
        def exact(n):
            data=b''
            while len(data)<n:
                part=parent.recv(n-len(data)); assert part; data+=part
            return data
        def receive():
            magic,version,kind,size=struct.unpack('>IHHI',exact(12)); assert (magic,version,size)==(0x50574331,1,70)
            return kind,struct.unpack('>QQQIB32sQB',exact(size))
        def send(kind,permit): parent.sendall(struct.pack('>IHHI',0x50574331,1,kind,70)+struct.pack('>QQQIB32sQB',*permit))
        try:
            kind,ready=receive(); assert kind==1 and ready[-1]==1; session=ready[0]
            wrapper='import os,signal,sys; os.kill(os.getpid(),signal.SIGSTOP); os.execv(sys.argv[1],sys.argv[1:])'
            client=subprocess.Popen([sys.executable,'-c',wrapper,str(build/'tests/prism_skia_gles_wayland_probe'),'wayland-prism-0',str(root/'tests/fixtures/skia_probe.prism'),'prism_dock'],cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            deadline=time.monotonic()+5
            while '\nState:\tT' not in Path(f'/proc/{client.pid}/status').read_text():
                assert client.poll() is None and time.monotonic()<deadline
                time.sleep(.01)
            permit=(session,1,11,client.pid,3,os.urandom(32),time.monotonic_ns()+250000000,0)
            send(2,permit); kind,reply=receive(); assert kind==3 and reply[-1]==1
            send(2,permit); kind,reply=receive(); assert kind==3 and reply[-1]==0
            expired=(session,2,12,wm.pid,2,os.urandom(32),time.monotonic_ns()-1,0)
            send(2,expired); kind,reply=receive(); assert kind==3 and reply[-1]==0
            time.sleep(.35); os.kill(client.pid,signal.SIGCONT)
            output,error=client.communicate(timeout=10)
            assert client.returncode!=0,(output,error,log_path.read_text())
            assert 'registration expired or invalid' in error,(output,error,log_path.read_text())
            assert "Mapped app_id='prism_dock'" not in log_path.read_text()
            send(4,permit)
            dead=(session,3,13,client.pid,2,os.urandom(32),time.monotonic_ns()+1000000000,0)
            send(2,dead); kind,reply=receive(); assert kind==3 and reply[-1]==0
            assert wm.poll() is None,log_path.read_text()
            print('Duplicate/dead/expired grants rejected; a delayed registered Shell fails instead of mapping as ordinary; WM remains alive.')
        finally:
            if client and client.poll() is None: client.kill(); client.wait()
            wm.terminate()
            try: wm.wait(timeout=5)
            except subprocess.TimeoutExpired: wm.kill(); wm.wait()
            parent.close()
