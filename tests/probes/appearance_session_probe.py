#!/usr/bin/env python3
"""Exercise installed physical appearance transactions; never installed."""
import argparse
import json
import os
from pathlib import Path
import socket
import struct
import subprocess

parser=argparse.ArgumentParser()
parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args()
args.output.mkdir(parents=True,exist_ok=True)
env=dict(os.environ,XDG_RUNTIME_DIR='/run/user/'+str(os.getuid()))

def ipc(*words,ok=True):
    result=subprocess.run(['prism-msg','-r',*words],env=env,capture_output=True,text=True,timeout=15)
    assert (result.returncode==0)==ok,(words,result.stdout,result.stderr)
    return json.loads(result.stdout)

def wm_pid():
    with socket.socket(socket.AF_UNIX) as peer:
        peer.connect(env['XDG_RUNTIME_DIR']+'/prism-ipc.sock')
        return struct.unpack('3i',peer.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))[0]

def receive(peer,size):
    value=b''
    while len(value)<size:
        part=peer.recv(size-len(value));assert part;value+=part
    return value

def replay_identity():
    # Same ID/package with another scheme is another identity, even when the
    # first request was only a query. The owner must close this peer.
    runtime=Path(env['XDG_RUNTIME_DIR'])
    with socket.socket(socket.AF_UNIX) as peer:
        peer.settimeout(3);peer.connect(str(runtime/'prism/launcher.sock'))
        def send(scheme):
            body=struct.pack('>QH',91,0)
            if scheme:body+=struct.pack('>BH',1,len(scheme))+scheme.encode()
            peer.sendall(struct.pack('>IHHIQQ',0x50524c31,1,6,len(body),91,0)+body)
        send('')
        header=receive(peer,28)
        fields=struct.unpack('>IHHIQQ',header);assert fields[2]==7 and fields[4]==91
        receive(peer,fields[3])
        send('light')
        assert peer.recv(1)==b''

initial=ipc('get_theme')
original_pid=wm_pid()
original_windows=ipc('get_status')['windows_count']
results=[]
try:
    replay_identity()
    for scheme in ('light','dark'):
        previous=ipc('get_theme')
        applied=ipc('set_color_scheme',scheme)
        assert applied['id']==previous['id'] and applied['color_scheme']==scheme,applied
        results.append(applied)
        for theme in ('glass','translucent','transparent','square'):
            applied=ipc('set_theme',theme)
            assert applied['id']==theme and applied['color_scheme']==scheme,applied
            assert ipc('get_status')['theme']['color_scheme']==scheme
            assert wm_pid()==original_pid and ipc('get_status')['windows_count']==original_windows
            results.append(applied)
    before=ipc('get_theme')
    rejected=ipc('set_theme','missing-theme',ok=False)
    after=ipc('get_theme')
    assert rejected['status']=='rejected' and before['id']==after['id'] and before['color_scheme']==after['color_scheme']
    report={'wm_pid':original_pid,'windows':original_windows,'transactions':results,'unknown_theme':rejected,'replay_scheme_collision':'peer closed'}
    (args.output/'results.json').write_text(json.dumps(report,indent=2)+'\n')
finally:
    ipc('set_theme',initial['id'])
    ipc('set_color_scheme',initial['color_scheme'])
print('All eight material/palette combinations and selector replay identity passed.')
