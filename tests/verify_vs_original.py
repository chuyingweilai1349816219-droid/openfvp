#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""与原版**实拍帧**的回归门禁。

为什么单独一个门禁：`verify_gfx` 的基准来自我们自己的参考渲染器，只能证明"C 实现 ==
Python 原型"；真正要回答"和原版差多少"必须拿原版抓的帧来比。语料在 `out/`（不入库，
见 docs/original-verification.md §7），所以本脚本在缺语料时打印 SKIP 并返回 0。

每个用例在 `tests/original-baseline.json` 里描述：
  frame  原版帧路径（1926x1109 全窗位图，客户区从 offset 开始）
  args   我们外壳的重现参数（例如 --title-menu --title-font ...）
  baseline.mean_abs  冻结的实测平均绝对差（0..255）
  baseline.tol       允许的余量（默认 1.5），超过即判回归

用法：python tests/verify_vs_original.py
"""
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
OUT = os.path.join(FVP, 'out', 'vs-original')
GAMES = {
    'iroseka': (os.path.join(r'C:\Users\Mechrevo\Downloads',
                             '五彩斑斓的世界HD'), 'iroseka.hcb'),
    'hoshimemo': (os.path.join(r'C:\Users\Mechrevo\Downloads',
                               'hoshimemo_installed'), 'Hoshimemo_HD.hcb'),
}


def run(argv, timeout=900):
    proc = subprocess.run(argv, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=timeout)
    return proc.returncode, proc.stdout.decode('utf-8', 'replace')


def read_bmp(path):
    with open(path, 'rb') as f:
        data = f.read()
    if data[:2] != b'BM':
        return None
    off = struct.unpack_from('<I', data, 10)[0]
    w = struct.unpack_from('<i', data, 18)[0]
    h = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp != 32:
        return None
    return data, off, w, h


def bmp_pixels(path):
    got = read_bmp(path)
    if got is None:
        return None
    data, off, w, h = got
    out = []
    stride = w * 4
    for y in range(h):
        row = off + (h - 1 - y) * stride  # BMP 自下而上
        out.append([(data[row + x * 4 + 2], data[row + x * 4 + 1],
                     data[row + x * 4]) for x in range(w)])
    return out


def png_pixels(path, offset):
    import numpy as np
    from PIL import Image

    ox, oy = offset
    im = Image.open(path).convert('RGB').crop((ox, oy, ox + 1920,
                                              oy + 1080))
    a = np.asarray(im)
    return [[tuple(int(v) for v in a[y, x]) for x in range(1920)]
            for y in range(1080)]


def mean_abs_diff(a, b):
    total = 0
    n = 0
    for y in range(0, 1080, 2):
        ra = a[y]
        rb = b[y]
        for x in range(0, 1920, 2):
            pa = ra[x]
            pb = rb[x]
            total += (abs(pa[0] - pb[0]) + abs(pa[1] - pb[1]) +
                      abs(pa[2] - pb[2]))
            n += 3
    return total / float(n) if n else 0.0


def main():
    try:
        from PIL import Image  # noqa: F401
    except ImportError:
        print('verify_vs_original: SKIP (Pillow unavailable)')
        return 0
    spec_path = os.path.join(HERE, 'original-baseline.json')
    with open(spec_path, encoding='utf-8') as f:
        spec = json.load(f)
    os.makedirs(OUT, exist_ok=True)
    exe = os.path.join(FVP, 'bin', 'fvp.exe')
    if not os.path.isfile(exe):
        print('verify_vs_original: SKIP (bin/fvp.exe missing)')
        return 0

    fails = []
    ran = 0
    for case in spec['cases']:
        name = case['name']
        frame = os.path.join(FVP, case['frame'])
        game = GAMES.get(case['game'])
        if not os.path.isfile(frame) or game is None or not os.path.isdir(
                game[0]):
            print('  skip  %s (缺少原版帧或游戏目录)' % name)
            continue
        shot = os.path.join(OUT, name + '.bmp')
        log = os.path.join(OUT, name + '.log')
        for p in (shot, log):
            if os.path.isfile(p):
                os.remove(p)
        rc, _o = run([exe, '--no-input', '--game', game[0], '--script',
                      game[1], '--run-seconds', '3', '--no-audio', '--scale',
                      '1', '--screenshot', shot, '--log', log] + case['args'])
        if rc != 0 or not os.path.isfile(shot):
            print('  FAIL  %s: shell exit %s' % (name, rc))
            fails.append(name)
            continue
        ours = bmp_pixels(shot)
        theirs = png_pixels(frame, case['offset'])
        mean_abs = mean_abs_diff(ours, theirs)
        base = case['baseline']['mean_abs']
        tol = case.get('baseline', {}).get('tol', 1.5)
        ran += 1
        if case.get('assert', True):
            ok = mean_abs <= base + tol
            print(('  ok    ' if ok else '  FAIL  ') +
                  '%s: mean abs diff %.2f (baseline %.2f + %.1f)'
                  % (name, mean_abs, base, tol))
            if not ok:
                fails.append(name)
        else:
            print('  info  %s: mean abs diff %.2f (baseline %.2f, 不断言%s)'
                  % (name, mean_abs, base,
                     '：' + case['note'] if case.get('note') else ''))
    if ran == 0:
        print('verify_vs_original: SKIP (没有可用的原版帧语料，见 '
              'docs/original-verification.md §7)')
        return 0
    if fails:
        print('verify_vs_original: %d case(s) FAILED' % len(fails))
        return 1
    print('verify_vs_original: PASS (%d case(s))' % ran)
    return 0


if __name__ == '__main__':
    sys.exit(main())