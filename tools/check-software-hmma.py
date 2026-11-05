#!/usr/bin/env python3
"""Run the software HMMA host regression (RTX 4090 reference fragments) with the local Catch2 libraries."""
from pathlib import Path
import os
import subprocess
import sys
r = Path(__file__).resolve().parents[1]
c = r/'Eglantine'
# EDEN_CATCH2_BUILD: another checkout's .local/nx2-core-check (a worktree has none of its own).
b = Path(os.environ.get('EDEN_CATCH2_BUILD') or r/'.local/nx2-core-check')
import tempfile
work = tempfile.TemporaryDirectory(prefix='software-hmma-')
scratch = Path(work.name)
files = ['ampere_matrix.cpp']
args = ['clang++-18', '-std=c++20', '-O1', '-g', '-DARCHITECTURE_x86_64=1', '-pthread',
        '-fsanitize=address,undefined']
includes = [c/'src', b/'_deps/catch2-src/src', b/'_deps/catch2-build/generated-includes']
args += ['-I'+str(p) for p in includes]
args += [str(c/'src/tests/video_core'/p) for p in files]
args += [str(b/'_deps/catch2-build/src/libCatch2Main.a'),
         str(b/'_deps/catch2-build/src/libCatch2.a'), '-o', str(scratch/'check')]
subprocess.run(args, check=True)
result = subprocess.run([str(scratch/'check'), '--reporter', 'compact'], capture_output=True, text=True)
print(result.stdout[-6500:])
print(result.stderr[-1500:])
sys.exit(result.returncode)
