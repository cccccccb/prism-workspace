"""Installed DRM session functional gate. Does not start/stop the compositor.

Run before manual interaction in a fresh Prism session. Leaves one Music and
one Settings window for physical inspection; all launches go through launcher.
"""
import json
import os
from pathlib import Path
import socket
import struct
import sys
import time

MAGIC=0x50524C31
class Peer:
    def __init__(self, path):
        self.sock=socket.socket(socket.AF_UNIX)
        self.sock.connect(str(path)); self.sock.settimeout(.2)
        self.buffer=b''; self.events=[]; self.updates=[]
    def send(self, kind, request, body=b''):
        self.sock.sendall(struct.pack('>IHHIQQ',MAGIC,1,kind,len(body),request,0)+body)
    def launch(self, request, app, mode=1):
        data=app.encode(); self.send(1,request,struct.pack('>BH',mode,len(data))+data)
    def wait(self, predicate, timeout=20):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if predicate(): return
            if len(self.buffer)>=28:
                magic,version,kind,n,request,instance=struct.unpack('>IHHIQQ',self.buffer[:28])
                assert (magic,version)==(MAGIC,1) and n<=65536
                if len(self.buffer)>=28+n:
                    body=self.buffer[28:28+n]; self.buffer=self.buffer[28+n:]
                    if kind==2:
                        pid,milestone,error,code,length=struct.unpack('>IBHiH',body[:13])
                        assert len(body)==13+length
                        self.events.append(dict(request=request,instance=instance,pid=pid,milestone=milestone,
                            error=error,exit=code,detail=body[13:].decode(),received_ns=time.monotonic_ns()))
                    else:
                        assert kind==5
                        pid,change,length=struct.unpack('>IBH',body[:7]); assert len(body)==7+length
                        self.updates.append(dict(request=request,instance=instance,pid=pid,change=change,app=body[7:].decode()))
                    continue
            try: data=self.sock.recv(65536)
            except socket.timeout: continue
            assert data,(self.events,self.updates)
            self.buffer+=data
        raise AssertionError((self.events,self.updates))
    def has(self, request, milestone):
        return any(e['request']==request and e['milestone']==milestone for e in self.events)
    def ready(self, request): return self.has(request,4) and self.has(request,5)
    def event(self, request, milestone):
        return next(e for e in self.events if e['request']==request and e['milestone']==milestone)

runtime=Path(os.environ['XDG_RUNTIME_DIR'])
peer=Peer(runtime/'prism/launcher.sock')
started={}; report={}
try:
    peer.send(4,1010)
    peer.wait(lambda:any(u['change']==3 for u in peer.updates))
    assert not any(u['change']==1 for u in peer.updates),'Run in a fresh session before launching apps'
    for request,app in ((1001,'demo_player'),(1003,'demo_player'),(1004,'demo_settings')):
        if request==1003:
            peer.launch(1002,'demo_player',0); peer.wait(lambda:peer.has(1002,8))
            activation=[e for e in peer.events if e['request']==1002]
            original=peer.event(1001,1)
            assert [e['milestone'] for e in activation]==[0,1,8],activation
            assert activation[-1]['pid']==original['pid'] and activation[-1]['instance']==original['instance']
            peer.send(3,1002)
            peer.wait(lambda:sum(e['request']==1002 and e['milestone']==8 for e in peer.events)>=2)
            assert Path(f"/proc/{original['pid']}").exists()
        started[request]=time.monotonic_ns(); peer.launch(request,app)
        peer.wait(lambda:peer.ready(request))
        shown=peer.event(request,4)
        assert shown['detail'].startswith('GL renderer=V3D'),shown
        peer.wait(lambda:any(u['instance']==shown['instance'] and u['change']==1 for u in peer.updates))
        print(f"{app}: instance={shown['instance']} pid={shown['pid']} {shown['detail']}",flush=True)
    second=peer.event(1003,1); original=peer.event(1001,1)
    assert second['pid']!=original['pid'] and second['instance']!=original['instance']
    peer.send(3,1003); peer.wait(lambda:peer.has(1003,7))
    peer.wait(lambda:any(u['instance']==second['instance'] and u['change']==2 for u in peer.updates))
    assert not Path(f"/proc/{second['pid']}").exists()
    peer.launch(1005,'prism_dock'); peer.wait(lambda:peer.has(1005,6))
    assert peer.event(1005,6)['error']==1 and peer.event(1005,6)['pid']==0
    # Verify a late subscriber receives exactly the two remaining managed windows.
    late=Peer(runtime/'prism/launcher.sock')
    try:
        late.send(4,1); late.wait(lambda:any(u['change']==3 for u in late.updates))
        snapshot=[u for u in late.updates if u['change']==1]
        assert sorted(u['app'] for u in snapshot)==['demo_player','demo_settings'],snapshot
        report['remaining']=snapshot
    finally: late.sock.close()
    report.update(events=peer.events,updates=peer.updates,
        startup_ms={str(r):{str(m):(peer.event(r,m)['received_ns']-start)/1e6 for m in (1,4,5)} for r,start in started.items()},
        note='Single-run functional observations, not performance percentiles. Physical visibility/input requires user confirmation.')
    if len(sys.argv)>1:
        output=Path(sys.argv[1]); output.parent.mkdir(parents=True,exist_ok=True)
        output.write_text(json.dumps(report,indent=2)+'\n')
    print('DRM first presentation/ready, same-instance activation, NewInstance, cancellation, reserved Shell rejection and real instance stream passed.',flush=True)
finally:
    peer.sock.close() # Accepted applications outlive their request connection.
