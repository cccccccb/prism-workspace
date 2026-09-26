"""V3D lower-scene blur regression; isolated fixtures, never production assets.

Usage: python3 tests/probes/backdrop_probe.py BUILD GRIM OUTPUT_DIRECTORY
Requires a native grim binary (may be unpacked from Debian package).
Checks high-frequency lower content gets blurred and lower updates propagate.
"""
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

root=Path(__file__).resolve().parents[2]
build=Path(sys.argv[1]).resolve();grim=Path(sys.argv[2]).resolve();output=Path(sys.argv[3]).resolve();output.mkdir(parents=True,exist_ok=True)
def png(path,width,height):
    def chunk(kind,data):return struct.pack('>I',len(data))+kind+data+struct.pack('>I',zlib.crc32(kind+data)&0xffffffff)
    row=b'\x00'+b''.join(bytes([255 if x%4<2 else 0])*3 for x in range(width))
    path.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(row*height))+chunk(b'IEND',b''))
def ppm(path):
    data=path.read_bytes();head,rest=data.split(b'\n',1);assert head==b'P6'
    while rest.startswith(b'#'):rest=rest.split(b'\n',1)[1]
    dims,rest=rest.split(b'\n',1);width,height=map(int,dims.split());depth,data=rest.split(b'\n',1);assert depth==b'255';assert len(data)==width*height*3
    return width,height,data

def patch(image,x,y,w,h):
    width,height,data=image
    return [tuple(data[(yy*width+xx)*3:(yy*width+xx)*3+3]) for yy in range(y,y+h) for xx in range(x,x+w)]
with tempfile.TemporaryDirectory(prefix='prism-backdrop-') as tmp:
    directory=Path(tmp);runtime=directory/'runtime';runtime.mkdir(mode=0o700)
    apps=directory/'apps';shutil.copytree(build/'share/prism/apps',apps)
    desktop=apps/'prism_desktop';assets=desktop/'assets';assets.mkdir(exist_ok=True)
    png(assets/'checker.png',1024,600)
    (desktop/'master.prism').write_text('Card { Image("checker.png", fit: "fill") Card(background: $testTint) }')
    subprocess.run(['cc','-shared','-fPIC','-I'+str(root/'prism/include'),str(root/'tests/fixtures/backdrop_module.c'),'-o',str(desktop/'desktop.so')],check=True)
    env=dict(os.environ,XDG_RUNTIME_DIR=str(runtime),WAYLAND_DISPLAY='wayland-prism-0',WLR_BACKENDS='headless',WLR_RENDERER='gles2')
    env.pop('WAYLAND_SOCKET',None)
    for node in Path('/sys/class/drm').glob('renderD*'):
        driver=node/'device/driver'
        if driver.exists() and driver.resolve().name=='v3d':env['WLR_RENDER_DRM_DEVICE']='/dev/dri/'+node.name
    log_path=output/'backdrop.log'
    with log_path.open('w') as log:
        session=subprocess.Popen([str(build/'bin/prism-session-runtime'),'--apps-root',str(apps)],env=env,stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+20
            while not (runtime/'prism/launcher.sock').exists() or log_path.read_text().count('milestone=5')<3:
                assert session.poll() is None,log_path.read_text();assert time.monotonic()<deadline,log_path.read_text();time.sleep(.05)
            mode=subprocess.run([str(build/'prism/prism-msg'),'-r','mode','HEADLESS-1','1024','600'],env=env,capture_output=True,text=True,timeout=5)
            assert json.loads(mode.stdout)['status']=='ok',mode.stdout+mode.stderr
            time.sleep(.2)
            snapshots=[]
            for n in range(8):
                capture=output/f'backdrop-{n}.ppm'
                # Keep protocol progress when a capture stalls. A successful
                # compositor frame callback is not proof of an output-buffer
                # commit; do not retry and accidentally hide that distinction.
                trace_path=output/f'capture-{n}-wayland.log'
                started=time.monotonic()
                capture_env=dict(env)
                if os.environ.get('PRISM_CAPTURE_WAYLAND_DEBUG')=='1':
                    capture_env['WAYLAND_DEBUG']='client'
                else:
                    capture_env.pop('WAYLAND_DEBUG',None)
                with trace_path.open('w') as trace:
                    process=subprocess.Popen([str(grim),'-t','ppm',str(capture)],
                        env=capture_env,stdout=trace,stderr=trace)
                    try:
                        result=process.wait(timeout=8)
                        if result:raise subprocess.CalledProcessError(result,process.args)
                    except subprocess.TimeoutExpired:
                        proc=Path('/proc')/str(process.pid)
                        details={}
                        for name in ('wchan','syscall','status'):
                            try:details[name]=(proc/name).read_text()
                            except OSError as error:details[name]=str(error)
                        try:details['fds']={fd.name:os.readlink(fd) for fd in (proc/'fd').iterdir()}
                        except OSError as error:details['fds']=str(error)
                        try:
                            state=subprocess.run([str(build/'prism/prism-msg'),'-r','get_outputs'],
                                env=env,capture_output=True,text=True,timeout=3)
                            state_stdout,state_stderr=state.stdout,state.stderr
                        except subprocess.TimeoutExpired:
                            state_stdout,state_stderr='','Output diagnostic request also timed out'
                        (output/f'capture-{n}-timeout.json').write_text(json.dumps(dict(
                            capture=n,elapsed=time.monotonic()-started,
                            capture_pid=process.pid,capture_process=details,
                            capture_bytes=capture.stat().st_size if capture.exists() else None,
                            session_pid=session.pid,session_exit=session.poll(),
                            outputs_stdout=state_stdout,outputs_stderr=state_stderr,
                            protocol_trace=str(trace_path)),indent=2)+'\n')
                        raise
                    finally:
                        if process.poll() is None:
                            process.kill();process.wait()
                image=ppm(capture);assert image[:2]==(1024,600)
                glass=patch(image,300,24,80,8);lower=patch(image,300,80,80,8)
                glass_range=max(p[0] for p in glass)-min(p[0] for p in glass)
                lower_range=max(p[0] for p in lower)-min(p[0] for p in lower)
                mean=[sum(p[c] for p in glass)/len(glass) for c in range(3)]
                assert lower_range>80,(n,lower_range)
                assert glass_range<lower_range*.25,(n,glass_range,lower_range)
                snapshots.append(dict(glass_range=glass_range,lower_range=lower_range,glass_mean=mean))
                time.sleep(.2)
            # Red/blue tint alternates in the LOWER client. The topbar stays unchanged.
            tones=[s['glass_mean'][0]-s['glass_mean'][2] for s in snapshots]
            assert max(tones)-min(tones)>8,tones
            journal=log_path.read_text()
            assert 'GPU backdrop capability=1' in journal and 'blur=12.0 lower-scene-only' in journal
            # A headless output has no backend DRM FD for dmabuf feedback.
            # Keep every other error (including material/commit errors) fatal.
            errors=[line for line in journal.splitlines() if 'ERROR' in line
                    and '[types/wlr_linux_dmabuf_v1.c:1240] Failed to get backend DRM FD' not in line]
            assert 'GL renderer=V3D' in journal and not errors,journal
            (output/'backdrop-report.json').write_text(json.dumps(dict(snapshots=snapshots,lower_update_delta=max(tones)-min(tones)),indent=2)+'\n')
            print('V3D blur attenuated lower checker pattern and refreshed after actual lower-surface changes.')
        finally:
            session.terminate()
            try:assert session.wait(timeout=8)==0
            except subprocess.TimeoutExpired:session.kill();session.wait();raise
