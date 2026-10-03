"""Real Pi V3D session / trusted-role / activation integration; never installed."""
import os
import ctypes
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1] if len(sys.argv)>1 else root/'build-gles').resolve()
MAGIC=0x50524c31
class Peer:
    def __init__(self,path):
        self.sock=socket.socket(socket.AF_UNIX); self.sock.connect(str(path)); self.sock.settimeout(.1)
        self.buffer=b''; self.events=[]; self.updates=[]
    def send(self,kind,request,body=b''):
        self.sock.sendall(struct.pack('>IHHIQQ',MAGIC,1,kind,len(body),request,0)+body)
    def launch(self,request,app,mode=0):
        data=app.encode(); self.send(1,request,struct.pack('>BH',mode,len(data))+data)
    def wait(self,predicate,timeout=15):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if predicate(): return
            if len(self.buffer)>=28:
                magic,version,kind,n,request,instance=struct.unpack('>IHHIQQ',self.buffer[:28])
                assert (magic,version)==(MAGIC,1)
                if len(self.buffer)>=28+n:
                    body=self.buffer[28:28+n]; self.buffer=self.buffer[28+n:]
                    if kind==2:
                        pid,milestone,error,code,size=struct.unpack('>IBHiH',body[:13]); assert len(body)==13+size
                        self.events.append(dict(request=request,instance=instance,pid=pid,milestone=milestone,error=error,exit=code))
                    else:
                        assert kind==5
                        pid,change,size=struct.unpack('>IBH',body[:7]); assert len(body)==7+size
                        self.updates.append(dict(request=request,instance=instance,pid=pid,change=change,app=body[7:].decode()))
                    continue
            try: data=self.sock.recv(65536)
            except socket.timeout: continue
            assert data,(self.events,self.updates); self.buffer+=data
        raise AssertionError((self.events,self.updates))
    def has(self,request,milestone): return any(e['request']==request and e['milestone']==milestone for e in self.events)
    def ready(self,request): return self.has(request,4) and self.has(request,5)
    def close(self): self.sock.close()
def wait_for(predicate,detail,timeout=15):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if predicate(): return
        time.sleep(.05)
    raise AssertionError(detail())
def children(pid):
    file=Path(f'/proc/{pid}/task/{pid}/children')
    return [int(p) for p in file.read_text().split()] if file.exists() else []

def shell_recovery(session, wm, launcher, peer, original, log_path):
    """A failed Shell loses its grant; unrelated windows and launch remain live."""
    initial = log_path.read_text()
    for request, (app, role) in enumerate(
            [('prism_topbar', 2), ('prism_dock', 3), ('prism_desktop', 1),
             ('prism_layout_controls', 4)], 20):
        shell = int(re.search(rf'Client pid=(\d+) authorized shell role={role}', initial)[1])
        os.kill(shell, signal.SIGKILL)
        wait_for(lambda: not Path(f'/proc/{shell}').exists(), log_path.read_text)
        wait_for(lambda: f'shell unavailable app={app}' in log_path.read_text(), log_path.read_text)

        assert session.poll() is None, log_path.read_text()
        assert all(Path(f'/proc/{pid}').exists() for pid in (wm, launcher, original['pid']))
        peer.launch(request, 'demo_player')
        peer.wait(lambda: peer.has(request, 8))
        activated = next(e for e in peer.events if e['request'] == request and e['milestone'] == 8)
        assert (activated['instance'], activated['pid']) == (original['instance'], original['pid'])

        peer.launch(request + 10, 'demo_settings', 1)
        peer.wait(lambda: peer.ready(request + 10))
        peer.send(3, request + 10)
        peer.wait(lambda: peer.has(request + 10, 7))
        assert session.poll() is None, log_path.read_text()
        assert len(re.findall(rf'authorized shell role={role}\b', log_path.read_text())) == 1

    assert log_path.read_text().count('policy=keep-session restart=disabled') == 4
    print('shell recovery: Topbar/Dock/Desktop/LayoutControls crashes preserved WM, applications and new launches; no automatic restarts')

probe_path=build/'tests/prism_skia_gles_wayland_probe'
if not probe_path.exists(): probe_path=root/'build-gles/tests/prism_skia_gles_wayland_probe'
inherit_helper='--inherited-helper' in sys.argv[2:]
if inherit_helper: assert ctypes.CDLL(None).prctl(36,1,0,0,0)==0
shutdowns=('normal','wm_crash','launcher_crash','shell_crash')
if '--normal-only' in sys.argv[2:]: shutdowns=('normal',)
if '--shell-recovery-only' in sys.argv[2:]: shutdowns=('shell_crash',)
for shutdown in shutdowns:
    with tempfile.TemporaryDirectory(prefix='prism-session-') as directory:
        runtime=Path(directory); log_path=runtime/'session.log'
        env=dict(os.environ,XDG_RUNTIME_DIR=directory,WLR_BACKENDS='headless',WLR_RENDERER='gles2',WAYLAND_DISPLAY='wayland-prism-0')
        env.pop('WAYLAND_SOCKET',None)
        for node in Path('/sys/class/drm').glob('renderD*'):
            if (node/'device/driver').exists() and (node/'device/driver').resolve().name=='v3d':
                env['WLR_RENDER_DRM_DEVICE']='/dev/dri/'+node.name
        peer=None; pids=[]
        with log_path.open('w+') as log:
            command=[str(build/'bin/prism-session-runtime'),'--wm',str(build/'bin/prism-wm')]
            if inherit_helper: command=[sys.executable,str(root/'tests/fixtures/session_parent.py')]+command
            session=subprocess.Popen(command,cwd=directory,env=env,stdout=log,stderr=log)
            try:
                def booted():
                    assert session.poll() is None,log_path.read_text()
                    return all(f"Mapped app_id='{app}' shell role={role}" in log_path.read_text()
                        for app,role in [('prism_desktop',1),('prism_topbar',2),('prism_dock',3),
                                             ('prism_layout_controls',4)])
                wait_for(booted,log_path.read_text,20)
                content=log_path.read_text(); match=re.search(r'session wm=(\d+) launcher=(\d+)',content); assert match,content
                wm,launcher=map(int,match.groups()); pids=[wm,launcher]+children(launcher)
                private={os.readlink(f'/proc/{p}/fd/3') for p in (wm,launcher)}
                for worker in children(launcher):
                    assert Path(f'/proc/{worker}/exe').resolve()==build/'bin/prism-app-host'
                    for fd in Path(f'/proc/{worker}/fd').iterdir():
                        try: assert os.readlink(fd) not in private
                        except FileNotFoundError: pass
                peer=Peer(runtime/'prism/launcher.sock')
                peer.send(4,100); peer.wait(lambda:any(u['change']==3 for u in peer.updates))
                assert [u['change'] for u in peer.updates]==[0,3],peer.updates
                peer.launch(1,'demo_player'); peer.wait(lambda:peer.ready(1))
                original=next(e for e in peer.events if e['request']==1 and e['milestone']==1)
                peer.wait(lambda:any(u['instance']==original['instance'] and u['change']==1 for u in peer.updates))
                peer.launch(2,'demo_player'); peer.wait(lambda:peer.has(2,8))
                activation=[e for e in peer.events if e['request']==2]
                assert [e['milestone'] for e in activation]==[0,1,8],activation
                assert activation[-1]['pid']==original['pid'] and activation[-1]['instance']==original['instance']
                # Cancelling completed activation cannot terminate the original instance.
                peer.send(3,2); peer.wait(lambda:sum(e['request']==2 and e['milestone']==8 for e in peer.events)>=2)
                assert Path(f"/proc/{original['pid']}").exists()
                if shutdown=='normal':
                    peer.launch(3,'demo_player',1); peer.wait(lambda:peer.ready(3))
                    second=next(e for e in peer.events if e['request']==3 and e['milestone']==1)
                    assert second['pid']!=original['pid'] and second['instance']!=original['instance']
                    peer.launch(4,'prism_topbar',1); peer.wait(lambda:peer.has(4,6))
                    assert next(e for e in peer.events if e['request']==4)['error']==1
                    peer.launch(5,'prism_layout_controls',1); peer.wait(lambda:peer.has(5,6))
                    assert next(e for e in peer.events if e['request']==5)['error']==1
                    result=subprocess.run([str(build/'bin/prism-invoker'),'demo_settings'],env=env,cwd=directory,capture_output=True,text=True,timeout=20)
                    assert result.returncode==0,result.stdout+result.stderr
                    result=subprocess.run([str(build/'bin/prism-invoker'),'demo_settings'],env=env,cwd=directory,capture_output=True,text=True,timeout=20)
                    assert result.returncode==0 and 'milestone=8' in result.stdout,result.stdout+result.stderr
                    # External Wayland app_id spoofing has no privileged role.
                    probe=subprocess.run([str(probe_path),'wayland-prism-0',str(root/'tests/fixtures/skia_probe.prism'),'prism_topbar'],env=env,cwd=root,capture_output=True,text=True,timeout=25)
                    assert probe.returncode==0,probe.stdout+probe.stderr
                    assert "Mapped app_id='prism_topbar' shell role=0" in log_path.read_text()
                    # A late subscriber gets real mapped instances, excluding shell/alias/spoof.
                    late=Peer(runtime/'prism/launcher.sock'); late.send(4,100)
                    try:
                        late.wait(lambda:any(u['change']==3 for u in late.updates))
                        active=[u for u in late.updates if u['change']==1]
                        assert len(active)==3 and sorted(u['app'] for u in active)==['demo_player','demo_player','demo_settings'],active
                    finally: late.close()
                    peer.send(3,1); peer.wait(lambda:peer.has(1,7))
                    peer.wait(lambda:any(u['instance']==original['instance'] and u['change']==2 for u in peer.updates))
                    assert Path(f"/proc/{second['pid']}").exists()
                if shutdown == 'shell_crash':
                    shell_recovery(session, wm, launcher, peer, original, log_path)
                pids=list(set(pids+children(launcher)))
                if shutdown in ('normal', 'shell_crash'): session.terminate()
                else: os.kill(wm if shutdown=='wm_crash' else launcher,signal.SIGKILL)
                code=session.wait(timeout=10)
                if inherit_helper:
                    helper=int((runtime/'inherited-helper.pid').read_text()); os.waitpid(helper,0)
                    assert not Path(f'/proc/{helper}').exists()
                assert code==(0 if shutdown in ('normal', 'shell_crash') else 1),(code,log_path.read_text())
                wait_for(lambda:all(not Path(f'/proc/{p}').exists() for p in pids),lambda: str(pids),5)
                if shutdown!='launcher_crash': assert not (runtime/'prism/launcher.sock').exists()
                print(f'{shutdown}: shell grants, real activation identity, instance stream and complete process cleanup passed')
            except Exception:
                print(log_path.read_text(),file=sys.stderr); raise
            finally:
                if peer: peer.close()
                if session.poll() is None:
                    session.terminate()
                    try: session.wait(timeout=10)
                    except subprocess.TimeoutExpired: session.kill(); session.wait()
