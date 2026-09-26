"""Compatibility entry for the unified session/client integration gate."""
from pathlib import Path
import runpy
runpy.run_path(str(Path(__file__).with_name("session_launch_probe.py")),run_name="__main__")
