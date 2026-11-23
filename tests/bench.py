#!/usr/bin/env python3
"""Benchmark: C VM vs frozen Python reference.

    python fvp/tests/bench.py [--steps 900000] [--repeat 3]

Runs both implementations on iroseka.hcb with the acceptance parameters and
prints wall-clock times per run (including process start, script load and decode).
`--steps 0` measures load+decode only.
"""
import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.abspath(os.path.join(HERE, '..'))
EXE = os.path.join(FVP, 'bin', 'fvm.exe')
REF_PY = os.path.join(FVP, 'ref', 'fvm_ref.py')
SCRIPT = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'


def run_once(cmd):
    t0 = time.perf_counter()
    proc = subprocess.run(cmd, stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL)
    dt = time.perf_counter() - t0
    if proc.returncode != 0:
        raise SystemExit('command failed (%d): %s' % (proc.returncode, cmd))
    return dt * 1000.0


def stats(name, cmd, repeat):
    times = [run_once(cmd) for _ in range(repeat)]
    med = statistics.median(times)
    print('%-14s median %8.1f ms   min %8.1f ms   runs=%s' % (
        name, med, min(times),
        ', '.join('%.1f' % t for t in times)))
    return med


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=900000)
    ap.add_argument('--click', type=int, default=600)
    ap.add_argument('--repeat', type=int, default=3)
    ap.add_argument('--script', default=SCRIPT)
    args = ap.parse_args()

    outdir = os.path.join(FVP, 'out', 'c')
    os.makedirs(outdir, exist_ok=True)
    c_out = os.path.join(outdir, 'bench-c.txt')
    py_out = os.path.join(outdir, 'bench-py.txt')

    c_cmd = [EXE, args.script, '--steps', str(args.steps),
             '--click', str(args.click), '--quiet', '--text-out', c_out]
    py_cmd = [sys.executable, REF_PY, args.script, '--steps', str(args.steps),
              '--click', str(args.click), '--quiet', '--text-out', py_out]

    print('script: %s' % args.script)
    print('steps : %d, click=%d' % (args.steps, args.click))
    c_med = stats('C fvm.exe', c_cmd, args.repeat)
    p_med = stats('Python ref', py_cmd, max(1, args.repeat // 2))
    print('speedup: %.1fx (C is faster)' % (p_med / c_med))

    if os.path.exists(c_out) and os.path.exists(py_out):
        a = open(c_out, 'rb').read()
        b = open(py_out, 'rb').read()
        print('outputs identical: %s (%d vs %d bytes)' % (
            a == b, len(a), len(b)))


if __name__ == '__main__':
    main()