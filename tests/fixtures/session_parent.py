"""Exec a supervisor with a pre-existing child, as systemd/PAM does."""
import os
from pathlib import Path
import subprocess
import sys
code='''import ctypes,os,signal,sys
parent=int(sys.argv[1])
if ctypes.CDLL(None).prctl(1,signal.SIGKILL,0,0,0) or os.getppid()!=parent: raise SystemExit(1)
while True: signal.pause()
'''
helper=subprocess.Popen([sys.executable,'-c',code,str(os.getpid())],stdin=subprocess.DEVNULL,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
Path(os.environ['XDG_RUNTIME_DIR'],'inherited-helper.pid').write_text(str(helper.pid))
os.execv(sys.argv[1],sys.argv[1:])
