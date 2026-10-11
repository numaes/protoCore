#!/usr/bin/env python3
"""Exit when the machine is quiet (fm.quiet_gate); print the reading."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fm
print(fm.quiet_gate())
