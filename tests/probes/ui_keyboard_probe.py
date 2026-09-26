#!/usr/bin/env python3
"""Send bounded UI navigation through a real uinput keyboard; never installed."""
import argparse
import fcntl
import os
from pathlib import Path
import struct
import time

parser=argparse.ArgumentParser()
parser.add_argument('--keys',required=True,help='comma-separated tab, enter, space, escape')
args=parser.parse_args()
keys={'tab':15,'enter':28,'space':57,'escape':1}
sequence=[keys[key.strip()] for key in args.keys.split(',')]
assert len(sequence)<=32

def ioctl(direction,number,size=0):
    return (direction<<30)|(size<<16)|(ord('U')<<8)|number
fd=os.open('/dev/uinput',os.O_WRONLY|os.O_NONBLOCK|os.O_CLOEXEC)
created=False
try:
    fcntl.ioctl(fd,ioctl(1,100,4),1) # EV_KEY
    for key in keys.values():fcntl.ioctl(fd,ioctl(1,101,4),key)
    fcntl.ioctl(fd,ioctl(1,3,92),struct.pack('HHHH80sI',3,0x5052,2,1,b'Prism test UI keyboard',0))
    fcntl.ioctl(fd,ioctl(0,1));created=True
    deadline=time.monotonic()+4
    while time.monotonic()<deadline:
        if any(path.read_text().strip()=='Prism test UI keyboard' for path in Path('/sys/class/input').glob('event*/device/name')):break
        time.sleep(.02)
    else:raise RuntimeError('uinput keyboard was not discovered')
    time.sleep(1)
    event=struct.Struct('@llHHi')
    for key in sequence:
        for value in (1,0):
            os.write(fd,event.pack(0,0,1,key,value)+event.pack(0,0,0,0,0))
            time.sleep(.08)
        time.sleep(.18)
finally:
    if created:fcntl.ioctl(fd,ioctl(0,2))
    os.close(fd)
print('UI key sequence sent; uinput keyboard removed.')
