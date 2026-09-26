"""Live DSL themes across the real unified V3D session. Never installed.
Usage: python3 tests/probes/theme_session_probe.py BUILD GRIM OUTPUT
"""
import json
import os
from pathlib import Path
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

build=Path(sys.argv[1]).resolve()
grim=Path(sys.argv[2]).resolve()
output=Path(sys.argv[3]).resolve()
output.mkdir(parents=True,exist_ok=True)
# A failed run must not leave a previous run's success report in this directory.
(output/'results.json').unlink(missing_ok=True)
(output/'capture-timeout.json').unlink(missing_ok=True)

def wait(predicate,detail,timeout=20):
    deadline=time.monotonic()+timeout
    while time.monotonic()<deadline:
        if predicate():return
        time.sleep(.04)
    raise AssertionError(detail())

with tempfile.TemporaryDirectory(prefix='prism-theme-') as directory:
    runtime=Path(directory)
    catalog=runtime/'themes'
    shutil.copytree(build/'share/prism/themes',catalog)
    broken=catalog/'broken'
    broken.mkdir()
    source=(catalog/'glass/theme.prism').read_text().replace('Theme("glass",','Theme("broken",')
    source=re.sub(r'^\s*Color\("text",[^\n]*\n','\n',source,flags=re.M)
    (broken/'theme.prism').write_text(source)
    env=dict(os.environ,XDG_RUNTIME_DIR=directory,WAYLAND_DISPLAY='wayland-prism-0',
             WLR_BACKENDS='headless',WLR_RENDERER='gles2')
    env.pop('WAYLAND_SOCKET',None)
    for node in Path('/sys/class/drm').glob('renderD*'):
        driver=node/'device/driver'
        if driver.exists() and driver.resolve().name=='v3d':
            env['WLR_RENDER_DRM_DEVICE']='/dev/dri/'+node.name
    log_path=output/'theme-session.log'
    results=[]
    def ipc(*args,success=True):
        r=subprocess.run([str(build/'prism/prism-msg'),'-r',*args],env=env,
                         capture_output=True,text=True,timeout=15)
        assert (r.returncode==0)==success,(r.stdout,r.stderr)
        return json.loads(r.stdout)
    def launch(app,new=False):
        r=subprocess.run([str(build/'bin/prism-invoker'),app]+(['--new'] if new else []),
                         env=env,capture_output=True,text=True,check=True,timeout=20)
        return int(re.search(r'pid=(\d+).*milestone=1',r.stdout)[1])
    def worker_ids():
        launcher=int(re.search(r'session wm=\d+ launcher=(\d+)',log_path.read_text())[1])
        return set(map(int,Path(f'/proc/{launcher}/task/{launcher}/children').read_text().split()))
    def installed(pid,generation):
        return f'worker theme installed pid={pid} generation={generation} ' in log_path.read_text()
    def identity_checks():
        def receive(sock,size):
            data=b''
            while len(data)<size:
                part=sock.recv(size-len(data));assert part;data+=part
            return data
        def frame(kind,request,body=b''):
            return struct.pack('>IHHIQQ',0x50524c31,1,kind,len(body),request,0)+body
        for collision in ('theme','subscription'):
            with socket.socket(socket.AF_UNIX) as peer:
                peer.settimeout(3);peer.connect(str(runtime/'prism/launcher.sock'))
                peer.sendall(frame(6,77,struct.pack('>QH',77,0)))
                header=receive(peer,28)
                magic,version,kind,length,request,instance=struct.unpack('>IHHIQQ',header)
                assert (magic,version,kind,request,instance)==(0x50524c31,1,7,77,0)
                receive(peer,length)
                if collision=='theme':
                    value=b'transparent';peer.sendall(frame(6,77,struct.pack('>QH',77,len(value))+value))
                else:peer.sendall(frame(4,77))
                assert peer.recv(1)==b'',collision
    def capture(name):
        time.sleep(.3)
        try:
            subprocess.run([str(grim),str(output/(name+'.png'))],env=env,check=True,timeout=10)
        except subprocess.TimeoutExpired:
            (output/'capture-timeout.json').write_text(json.dumps({
                'theme':name,'timeout_seconds':10,'automatic_retries':0,
                'session_pid':session.pid,'session_exit':session.poll(),
                'capture':str(output/(name+'.png')),'session_log':str(log_path),
            },indent=2)+'\n')
            raise
    with log_path.open('w') as log:
        session=subprocess.Popen([str(build/'bin/prism-session-runtime'),'--themes-root',str(catalog)],
                                 env=env,stdout=log,stderr=log)
        try:
            wait(lambda:log_path.read_text().count('milestone=4')>=3,log_path.read_text)
            assert session.poll() is None,log_path.read_text()
            assert ipc('mode','HEADLESS-1','1024','600')['status']=='ok'
            music=launch('demo_player');settings=launch('demo_settings')
            initial=ipc('get_theme');assert initial['id']=='glass' and initial['generation']==1,initial
            identity_checks()
            wait(lambda:len(worker_ids())==6,log_path.read_text)
            original=worker_ids()
            capture('glass')
            for name in ('translucent','transparent','square','glass'):
                result=ipc('set_theme',name)
                assert result['status']=='applied' and result['id']==name,result
                assert worker_ids()==original,(original,worker_ids())
                assert all(installed(pid,result['generation']) for pid in original),log_path.read_text()
                wm=ipc('get_status')['theme']
                assert wm['id']==name and wm['generation']==result['generation'],wm
                capture(name)
                results.append(result)
            before=ipc('get_theme')
            bad=ipc('set_theme','missing-theme',success=False)
            assert bad['status']=='rejected' and bad['generation']==before['generation'],bad
            # The compiler accepts this complete material set; live @text references reject it.
            rejected=ipc('set_theme','broken',success=False)
            assert rejected['status']=='rejected' and 'Host rejected' in rejected['detail'],rejected
            assert rejected['id']==before['id'] and rejected['generation']==before['generation']+2,rejected
            assert worker_ids()==original and all(installed(pid,rejected['generation']) for pid in original),log_path.read_text()
            result=ipc('set_theme','square')
            child=launch('demo_player',True)
            assert child not in (music,settings) and installed(child,result['generation']),log_path.read_text()
            assert Path(f'/proc/{music}').exists() and Path(f'/proc/{settings}').exists()
            capture('square-new-window')
            assert 'GL renderer=V3D' in log_path.read_text()
            results.extend([bad,rejected,result])
            (output/'results.json').write_text(json.dumps({'switches':results,'original_workers':sorted(original),
                'new_window_pid':child,'music_pid':music,'settings_pid':settings},indent=2)+'\n')
            print('Four live DSL themes, unchanged PIDs, compiler rejection, host rejection/rollback and new-window inheritance passed.')
        except Exception:
            print(log_path.read_text(),file=sys.stderr)
            raise
        finally:
            session.terminate()
            try:assert session.wait(timeout=8)==0
            except subprocess.TimeoutExpired:session.kill();session.wait();raise
