"""Capture real host applications in an isolated V3D session; never installed.

Usage: python3 tests/probes/visual_session_probe.py BUILD GRIM OUTPUT_DIRECTORY
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

build=Path(sys.argv[1]).resolve()
grim=Path(sys.argv[2]).resolve()
output=Path(sys.argv[3]).resolve()
output.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='prism-visual-') as directory:
    env=dict(os.environ,XDG_RUNTIME_DIR=directory,WAYLAND_DISPLAY='wayland-prism-0',
             WLR_BACKENDS='headless',WLR_RENDERER='gles2')
    env.pop('WAYLAND_SOCKET',None)
    for node in Path('/sys/class/drm').glob('renderD*'):
        driver=node/'device/driver'
        if driver.exists() and driver.resolve().name=='v3d':
            env['WLR_RENDER_DRM_DEVICE']='/dev/dri/'+node.name
    log_path=output/'visual-session.log'
    def ipc(*args):
        result=subprocess.run([str(build/'prism/prism-msg'),'-r',*args],env=env,
                              capture_output=True,text=True,check=True,timeout=5)
        return json.loads(result.stdout)
    def launch(app,new=False):
        subprocess.run([str(build/'bin/prism-invoker'),app]+(['--new'] if new else []),
                       env=env,capture_output=True,text=True,check=True,timeout=20)
    def capture(name):
        time.sleep(.3)
        subprocess.run([str(grim),str(output/(name+'.png'))],env=env,check=True,timeout=8)
        tree=ipc('get_tree')
        def check(node):
            if node.get('type')=='view':
                assert node['committed_rect']['width']>0 and node['committed_rect']['height']>0,node
            for child in node.get('nodes',[]):check(child)
            for child in node.get('workspaces',[]):check(child)
        check(tree)
        (output/(name+'-tree.json')).write_text(json.dumps(tree,indent=2)+'\n')
    with log_path.open('w') as log:
        session=subprocess.Popen([str(build/'bin/prism-session-runtime')],env=env,stdout=log,stderr=log)
        try:
            deadline=time.monotonic()+20
            while log_path.read_text().count('milestone=4')<3:
                assert session.poll() is None,log_path.read_text()
                assert time.monotonic()<deadline,log_path.read_text()
                time.sleep(.05)
            assert ipc('mode','HEADLESS-1','1024','600')['status']=='ok'
            launch('demo_player')
            launch('demo_settings')
            capture('two-windows')
            assert ipc('split','v')['status']=='ok'
            launch('demo_player',True)
            capture('three-windows')
            assert ipc('focus','left')['status']=='ok'
            assert ipc('split','v')['status']=='ok'
            launch('demo_settings',True)
            capture('four-windows')
            assert 'GL renderer=V3D' in log_path.read_text()
            print('Real host applications captured at 1024x600 in two, three and four BSP tiles.')
        finally:
            session.terminate()
            try:assert session.wait(timeout=8)==0
            except subprocess.TimeoutExpired:session.kill();session.wait();raise
