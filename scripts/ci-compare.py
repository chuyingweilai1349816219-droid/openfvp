#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ci-compare.py -- byte-exact acceptance test, with an honest "no data" mode.

This is the wrapper CI uses (see .github/workflows/ci.yml).  It does two things:

1. **Always** verifies the five frozen reference files in `ref/` against the
   SHA256 table in `ref/REFERENCE.md`.  This needs no game data and catches a
   baseline that was accidentally edited or line-ending-mangled.
2. **If a game script is available**, runs `tests/compare.py` with the
   acceptance parameters (900,000 steps, `--click 600`) and propagates its
   verdict: `PASS` / `FAIL`.

If no game data is available the equivalence run is **skipped on purpose**:

    RESULT: SKIPPED -- no game script; byte-equivalence NOT verified

The exit code is 0 (the reference check did pass), but the word SKIPPED is
printed so nobody can mistake it for a pass.  A missing exe or an explicitly
given-but-missing script is an error.

Usage
-----
    python scripts/ci-compare.py                     # refs only + skip compare
    python scripts/ci-compare.py --check-refs-only   # refs only, never compare
    python scripts/ci-compare.py --script GAME.hcb   # refs + real comparison
    FVP_SCRIPT=/path/to/iroseka.hcb python scripts/ci-compare.py

Exit codes: 0 = PASS or SKIPPED, 1 = FAIL, 2 = environment/usage error.
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
REF_DIR = os.path.join(ROOT, 'ref')
REFERENCE_MD = os.path.join(REF_DIR, 'REFERENCE.md')
COMPARE_PY = os.path.join(ROOT, 'tests', 'compare.py')

#: `| `file` | `SHA256` |` rows in ref/REFERENCE.md
REF_ROW_RE = re.compile(r'^\|\s*`([^`]+)`\s*\|\s*`([0-9A-Fa-f]{64})`')

#: Conventional place for a local game directory (never committed).
DEFAULT_SCRIPT_CANDIDATES = (
    os.path.join(ROOT, 'game', 'iroseka.hcb'),
)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def check_references():
    """Return (ok, checked, failures)."""
    if not os.path.isfile(REFERENCE_MD):
        return False, 0, ['missing %s' % os.path.relpath(REFERENCE_MD, ROOT)]
    rows = []
    with open(REFERENCE_MD, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            m = REF_ROW_RE.match(line.strip())
            if m:
                rows.append((m.group(1), m.group(2)))
    if not rows:
        return False, 0, ['no SHA256 rows parsed from ref/REFERENCE.md']

    failures = []
    for name, expected in rows:
        # Rows use the bare file name; all files live in ref/.
        path = os.path.join(REF_DIR, name.replace('\\', '/').split('/')[-1])
        if not os.path.isfile(path):
            failures.append('%s: file not found' % name)
            continue
        actual = sha256_file(path)
        if actual.lower() != expected.lower():
            failures.append(
                '%s: sha256 %s != expected %s' % (name, actual.upper(),
                                                  expected.upper()))
    return not failures, len(rows), failures


def find_script(explicit):
    if explicit:
        return os.path.abspath(explicit)
    env = os.environ.get('FVP_SCRIPT') or os.environ.get('FVP_GAME_SCRIPT')
    if env:
        return os.path.abspath(env)
    for cand in DEFAULT_SCRIPT_CANDIDATES:
        if os.path.isfile(cand):
            return cand
    return None


def default_exe():
    name = 'fvm.exe' if os.name == 'nt' else 'fvm'
    return os.path.join(ROOT, 'bin', name)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description='Reference-integrity check + byte-exact VM comparison.')
    ap.add_argument('--script', default=None,
                    help='path to the game .hcb (default: $FVP_SCRIPT, then '
                         'game/iroseka.hcb)')
    ap.add_argument('--exe', default=None,
                    help='path to the fvm executable (default: bin/fvm[.exe])')
    ap.add_argument('--steps', type=int, default=900000,
                    help='instruction budget (default 900000)')
    ap.add_argument('--click', type=int, default=600,
                    help='synthetic click period (default 600)')
    ap.add_argument('--check-refs-only', action='store_true',
                    help='only verify ref/ SHA256s; never run the comparison')
    args = ap.parse_args(argv)

    print('ci-compare: repository %s' % ROOT)

    ok, checked, failures = check_references()
    if ok:
        print('ref integrity: PASS (%d frozen files match ref/REFERENCE.md)'
              % checked)
    else:
        print('ref integrity: FAIL (%d problem(s))' % len(failures))
        for line in failures:
            print('  - %s' % line)
        print('\nHint: ref/*.txt must stay byte-exact (they are CRLF).  Do not '
              'let git convert line endings; check `git config core.autocrlf`.')
        print('\nRESULT: FAIL -- frozen reference files are not intact')
        return 1

    if args.check_refs_only:
        print('\nRESULT: PASS -- reference files intact (comparison not '
              'requested)')
        return 0

    script = find_script(args.script)
    if args.script and script and not os.path.isfile(script):
        print('ci-compare: game script not found: %s' % script,
              file=sys.stderr)
        print('\nRESULT: FAIL -- --script path does not exist')
        return 2

    if not script:
        print('byte-equivalence: no game script available '
              '(--script / $FVP_SCRIPT / game/iroseka.hcb)')
        print('\nRESULT: SKIPPED -- no game data; byte-equivalence was NOT '
              'verified')
        print('  Provide your own legally purchased copy and rerun with '
              '`--script /path/to/iroseka.hcb`.')
        return 0

    if not os.path.isfile(script):
        print('ci-compare: game script vanished: %s' % script, file=sys.stderr)
        return 2

    exe = os.path.abspath(args.exe) if args.exe else default_exe()
    if not os.path.isfile(exe):
        print('ci-compare: VM executable not found: %s' % exe, file=sys.stderr)
        print('  build it first: make            (POSIX)')
        print('                  make CC=gcc     (Windows / MSYS2)')
        print('\nRESULT: FAIL -- build artifact missing')
        return 2

    print('byte-equivalence: script=%s' % script)
    print('                  exe=%s' % exe)
    print('                  steps=%d click=%d' % (args.steps, args.click))

    cmd = [sys.executable, COMPARE_PY, '--run', '--script', script,
           '--exe', exe, '--steps', str(args.steps), '--click', str(args.click)]
    proc = subprocess.run(cmd)
    if proc.returncode == 0:
        print('\nRESULT: PASS -- byte-identical to the frozen reference')
        return 0
    if proc.returncode == 1:
        print('\nRESULT: FAIL -- output differs from the frozen reference')
        return 1
    print('\nRESULT: FAIL -- comparison could not run (exit code %d)'
          % proc.returncode, file=sys.stderr)
    return 2


if __name__ == '__main__':
    sys.exit(main())