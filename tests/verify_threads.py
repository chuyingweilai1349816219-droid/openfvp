#!/usr/bin/env python3
"""Coroutine slots (ThreadStart/ThreadExit) must not disturb the frozen baseline.

What it asserts
---------------
1. `fvm <script> --steps 900000 --click 600` (coroutine slots ON, the default)
   still prints exactly `ref/ref-900k.txt` -- byte for byte.
2. The run really did create coroutines, and every one of them retired:
   `coroutines on: start N, exit N` with N > 0.  (If a coroutine starts spinning
   instead of returning, the step budget is eaten and clause 1 fails first.)
3. `--no-threads` (the pre-coroutine stub) produces byte-identical text, i.e. the
   feature is behaviour-neutral on this title.  This is the honest claim the docs
   make in docs/05-threads.md 10 -- keep it machine-checked.
4. The 2M-step run (slots ON) still matches `ref/ref-2m.txt`.

Why this matters: the earlier Python prototype concluded that enabling threads
*reduces* dialogue (49 lines instead of 195).  The real cause was a VM bug -- the
bottom frame created by `initstack` returned to itself, so `ret` looped forever.
With that fixed, the script's coroutine bodies turn out to be compile-time
disabled (`pushint8 0; jmpcond <ret>`), so each thread starts and immediately
retires.  This test pins that down and will loudly fail if the semantics change.

Exit codes: 0 = pass, 1 = failure, 2 = missing prerequisites.

Usage:
    python fvp/tests/verify_threads.py
    python fvp/tests/verify_threads.py --steps 900000 --fast
"""
import argparse
import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
EXE = os.path.join(FVP, 'bin', 'fvm.exe')
REF_DIR = os.path.join(FVP, 'ref')
OUT_DIR = os.path.join(FVP, 'out')
DEFAULT_GAME = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD'
DEFAULT_SCRIPT = os.path.join(DEFAULT_GAME, 'iroseka.hcb')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run(args):
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def count_summary(text, key):
    for line in text.splitlines():
        if line.strip().startswith(key):
            return line.strip()
    return ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--script', default=DEFAULT_SCRIPT)
    ap.add_argument('--steps', type=int, default=900000)
    ap.add_argument('--fast', action='store_true', help='skip the 2M run')
    args = ap.parse_args()

    ref900 = os.path.join(REF_DIR, 'ref-900k.txt')
    ref2m = os.path.join(REF_DIR, 'ref-2m.txt')
    need = [EXE, args.script, ref900]
    if not args.fast:
        need.append(ref2m)
    for p in need:
        if not os.path.exists(p):
            print('verify_threads: missing %s' % p)
            return 2

    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    fails = []

    def check(cond, msg):
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        if not cond:
            fails.append(msg)

    on_txt = os.path.join(OUT_DIR, 'threads-on.txt')
    off_txt = os.path.join(OUT_DIR, 'threads-off.txt')

    print('== coroutine slots ON (default) @ %d steps ==' % args.steps)
    rc, out = run([EXE, args.script, '--steps', str(args.steps), '--click', '600',
                   '--quiet', '--text-out', on_txt])
    print(out.strip())
    check(rc == 0, 'fvm exit code 0 (no hang, no crash)')

    line = count_summary(out, 'coroutines')
    starts = exits = -1
    if 'start' in line and 'exit' in line:
        try:
            tail = line.split('start', 1)[1]
            starts = int(tail.split(',')[0].strip())
            exits = int(tail.split('exit', 1)[1].split(',')[0].strip())
        except (IndexError, ValueError):
            starts = exits = -1
    check('on' in line, 'coroutine slots reported as on')
    check(starts > 0, 'at least one ThreadStart created a coroutine (start=%d)'
          % starts)
    check(starts == exits,
          'every coroutine retired (start=%d, exit=%d)' % (starts, exits))

    same = os.path.exists(on_txt) and sha256(on_txt) == sha256(ref900)
    check(same, 'text is byte-identical to ref-900k.txt')

    print('== coroutine slots OFF (--no-threads) @ %d steps ==' % args.steps)
    rc, out = run([EXE, args.script, '--steps', str(args.steps), '--click', '600',
                   '--quiet', '--no-threads', '--text-out', off_txt])
    print(out.strip())
    check(rc == 0, 'fvm exit code 0 (no hang, no crash)')
    check('off' in count_summary(out, 'coroutines'),
          'coroutine slots reported as off')
    same = (os.path.exists(on_txt) and os.path.exists(off_txt)
            and sha256(on_txt) == sha256(off_txt))
    check(same, 'on/off text is byte-identical (behaviour-neutral)')

    if not args.fast:
        two = os.path.join(OUT_DIR, 'threads-2m.txt')
        print('== coroutine slots ON @ 2000000 steps ==')
        rc, out = run([EXE, args.script, '--steps', '2000000', '--click', '600',
                       '--quiet', '--text-out', two])
        print(out.strip())
        check(rc == 0, 'fvm exit code 0 (no hang, no crash)')
        same = os.path.exists(two) and sha256(two) == sha256(ref2m)
        check(same, 'text is byte-identical to ref-2m.txt')

    for p in (on_txt, off_txt):
        if os.path.exists(p):
            os.remove(p)

    print('')
    if not fails:
        print('verify_threads: PASS')
        return 0
    print('verify_threads: %d check(s) FAILED' % len(fails))
    return 1


if __name__ == '__main__':
    sys.exit(main())