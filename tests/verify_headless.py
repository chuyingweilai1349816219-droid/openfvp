#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""无窗口（headless）外壳门禁。

证明"核心 + 渲染层"在**不依赖 Win32 窗口 / D3D** 的情况下也能跑：
  1. `make headless` 能构建出 bin/fvp-headless.exe；
  2. 跑同一份脚本 N 条指令，步数与 fvm 完全一致；
  3. 输出的 BMP 是 1920x1080、不是全黑；
  4. 文本回调能写出至少一行。

用法：
    python tests/verify_headless.py [--steps N] [--script PATH]

缺游戏数据时返回 2（SKIP），与 verify_multigame 的约定一致。
"""
import argparse
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'headless')
DEFAULT_SCRIPT = os.path.join(
    r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD', 'iroseka.hcb')
sys.path.insert(0, HERE)

try:
    import harness_build as _hb
except Exception:  # pragma: no cover - harness always ships next to this file
    _hb = None


def find_make():
    """找 mingw32-make：优先用 gcc 同目录下的那个（run_all 的 PATH 很干净）。"""
    if _hb is not None:
        cc = _hb.find_cc()  # 返回的是命令 argv 列表，取第一个元素
        if cc:
            bindir = os.path.dirname(cc[0])
            for name in ('mingw32-make.exe', 'mingw32-make', 'make.exe',
                         'make'):
                cand = os.path.join(bindir, name)
                if os.path.isfile(cand):
                    return cand
    return 'mingw32-make'


MAKE = find_make()


def run(argv, timeout=1800):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace')


def bmp_stats(path):
    """解析 32 位 BMP 头并采样像素，返回 (w, h, mean)。"""
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        return None
    off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 24:
        return (w, h, -1.0)
    stride = (w * 3 + 3) & ~3
    total = 0
    count = 0
    for y in range(0, h, 37):
        row = off + y * stride
        for x in range(0, w, 41):
            total += (data[row + x * 3] + data[row + x * 3 + 1] +
                      data[row + x * 3 + 2])
            count += 1
    return (w, h, total / (3.0 * count) if count else 0.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=200000)
    ap.add_argument('--script', default=DEFAULT_SCRIPT)
    args = ap.parse_args()
    if not os.path.isfile(args.script):
        print('verify_headless: SKIP (missing script %s)' % args.script)
        return 2
    os.makedirs(OUT, exist_ok=True)
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if _hb is not None and _hb.find_cc() is None:
        print('verify_headless: SKIP (gcc unavailable)')
        return 0
    rc, out = run([MAKE, '-f', os.path.join(FVP, 'Makefile'), 'headless'])
    check(rc == 0, 'make headless exit 0 (got %s)' % rc)
    exe = os.path.join(FVP, 'bin', 'fvp-headless.exe')
    check(os.path.isfile(exe), 'bin/fvp-headless.exe exists')
    if not os.path.isfile(exe):
        print('verify_headless: 1 check failed (no binary)')
        return 1

    shot = os.path.join(OUT, 'frame.bmp')
    text = os.path.join(OUT, 'text.txt')
    for p in (shot, text):
        if os.path.isfile(p):
            os.remove(p)

    rc, out = run([exe, args.script, '--steps', str(args.steps),
                   '--screenshot', shot, '--text-out', text])
    check(rc == 0, 'headless run exit 0 (got %s)' % rc)
    check('platform seam = portable' in out,
          'headless reports the portable platform seam')
    steps = None
    for ln in out.splitlines():
        if ln.startswith('headless: steps='):
            steps = int(ln.split('=', 1)[1].split()[0])
    check(steps == args.steps,
          'headless ran %d steps (got %s)' % (args.steps, steps))

    fvm = os.path.join(FVP, 'bin', 'fvm.exe')
    if os.path.isfile(fvm):
        rc2, out2 = run([fvm, args.script, '--steps', str(args.steps),
                         '--quiet'])
        ref = None
        for ln in out2.splitlines():
            parts = ln.split()
            if len(parts) >= 2 and parts[0] == 'steps':
                ref = int(parts[1])
        check(ref == steps,
              'headless and fvm ran the same number of steps (%s vs %s)'
              % (steps, ref))

    stats = bmp_stats(shot) if os.path.isfile(shot) else None
    check(stats is not None, 'screenshot written')
    if stats:
        w, h, mean = stats
        check((w, h) == (1920, 1080),
              'screenshot is 1920x1080 (%dx%d)' % (w, h))
        check(mean > 1.0, 'screenshot is not black (mean %.1f)' % mean)

    n_lines = 0
    if os.path.isfile(text):
        with open(text, encoding='utf-8', errors='replace') as f:
            n_lines = sum(1 for _ in f)
    check(n_lines > 0, 'text callback produced %d line(s)' % n_lines)

    if fails:
        print('verify_headless: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_headless: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())