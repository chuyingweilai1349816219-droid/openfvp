#!/usr/bin/env python3
"""Byte-exact comparison between the C VM output and the frozen Python reference.

Default (main acceptance case):
    python fvp/tests/compare.py --run
runs fvm.exe on iroseka.hcb with --steps 900000 --click 600 --quiet and
compares the produced file with fvp/ref/ref-900k.txt byte by byte.

Long-run case:
    python fvp/tests/compare.py --run --steps 2000000

Exit code 0 iff both files are byte-identical.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.abspath(os.path.join(HERE, '..'))
REF_DIR = os.path.join(FVP, 'ref')
EXE = os.path.join(FVP, 'bin', 'fvm.exe')
OUT_DIR = os.path.join(FVP, 'out', 'c')
DEFAULT_SCRIPT = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'

REFS = {
    900000: os.path.join(REF_DIR, 'ref-900k.txt'),
    2000000: os.path.join(REF_DIR, 'ref-2m.txt'),
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def read_bytes(path):
    with open(path, 'rb') as f:
        return f.read()


def report(path, label):
    data = read_bytes(path)
    print('%s: %s' % (label, path))
    print('  bytes=%d  lines=%d  sha256=%s' % (
        len(data), data.count(b'\n'), sha256(path)))
    crlf = data.count(b'\r\n')
    print('  CRLF lines=%d  bare-LF=%d' % (crlf, data.count(b'\n') - crlf))
    return data


def first_diff(a, b):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    if len(a) != len(b):
        return n
    return -1


def context(data, off, width=32):
    lo = max(0, off - width)
    hi = min(len(data), off + width)
    return data[lo:hi]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=900000)
    ap.add_argument('--click', type=int, default=600)
    ap.add_argument('--enc', default='cp932')
    ap.add_argument('--script', default=DEFAULT_SCRIPT)
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--ref', default=None)
    ap.add_argument('--c', default=None, help='C output file to compare')
    ap.add_argument('--run', dest='run', action='store_true', default=True,
                    help='run fvm.exe before comparing (default)')
    ap.add_argument('--no-run', dest='run', action='store_false')
    ap.add_argument('--quiet', action='store_true', help='only print the verdict')
    args = ap.parse_args()

    ref = args.ref or REFS.get(args.steps)
    if ref is None:
        ap.error('no reference for --steps %d; pass --ref' % args.steps)
    out = args.c or os.path.join(OUT_DIR, 'c-text-%dk.txt' % (args.steps // 1000))
    if args.steps == 900000 and args.c is None:
        out = os.path.join(OUT_DIR, 'c-text.txt')

    if args.run:
        if not os.path.exists(args.exe):
            print('compare: missing %s (run fvp/build.ps1 first)' % args.exe,
                  file=sys.stderr)
            return 2
        if not os.path.exists(args.script):
            print('compare: missing script %s' % args.script, file=sys.stderr)
            return 2
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        cmd = [args.exe, args.script,
               '--steps', str(args.steps),
               '--click', str(args.click),
               '--enc', args.enc,
               '--quiet',
               '--text-out', out]
        t0 = time.perf_counter()
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
        elapsed = time.perf_counter() - t0
        if proc.returncode != 0:
            print('compare: fvm.exe failed with exit code %d' % proc.returncode,
                  file=sys.stderr)
            sys.stderr.write(proc.stderr.decode('utf-8', 'replace'))
            return 2
        if not args.quiet:
            print('C VM: %.1f ms, command:' % (elapsed * 1000.0))
            print('  ' + ' '.join(cmd))
            sys.stdout.write(proc.stdout.decode('utf-8', 'replace'))

    if not os.path.exists(ref):
        print('compare: missing reference %s' % ref, file=sys.stderr)
        return 2
    if not os.path.exists(out):
        print('compare: missing C output %s' % out, file=sys.stderr)
        return 2

    a = read_bytes(out)
    b = read_bytes(ref)
    if not args.quiet:
        report(out, 'C output')
        report(ref, 'reference')
    off = first_diff(a, b)
    if off < 0:
        print('RESULT: PASS -- byte-identical (%d bytes, %d lines)' % (
            len(a), a.count(b'\n')))
        print('  sha256 %s' % sha256(out))
        return 0

    line = a.count(b'\n', 0, min(off, len(a))) + 1
    print('RESULT: FAIL -- first difference at byte offset %d (line %d)' % (
        off, line))
    print('  C   : %r' % context(a, off))
    print('  REF : %r' % context(b, off))
    if len(a) != len(b):
        print('  length differs: C=%d REF=%d' % (len(a), len(b)))
    return 1


if __name__ == '__main__':
    sys.exit(main())