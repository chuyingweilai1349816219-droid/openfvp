#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图元树门禁：`--dump-prims` 导出的图元表必须"都挂在根组下"。

2026-11-27 的发现：脚本创建的图元以前默认 `parent = -1`（不在任何组里）。
窗口截图里标题画面只有背景、点不进去，`--dump-prims` 一导出就看见 135 个图元
里绝大部分 `parent=-1` —— 而 `gfx_prim_hit` 的判据（照原版 `0x43B904`/`0x43B8FB`）
要求父链能走到 **prim 0（根）**，`parent<0` 直接判未命中。

现在新建图元默认挂根组（0 号自己仍是 -1）。这条门禁钉住三件事：
  * 图元表导出可用（`--dump-prims`）；
  * 除 0 号外没有 `parent<0` 的图元；
  * 标题背景 250..261 在表里，且 `draw`/`alpha` 有值。

像素基线不受影响（本改动前后都是 289 px / 0.0139%）。
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
EXE = os.path.join(FVP, 'bin', 'fvp.exe')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')

RE_PRIM = re.compile(
    r'^prim id=(\d+) type=(\d+) draw=(\d+) alpha=(\d+) parent=(-?\d+) '
    r'xy=(-?\d+),(-?\d+) wh=(\d+),(\d+) graph=(-?\d+) tw=(-?\d+) z=(\d+)'
    r'(?: tex=(\S+))?')


def dump_prims():
    fd, log = tempfile.mkstemp(suffix='.log')
    os.close(fd)
    try:
        args = [EXE, '--game', GAME, '--title-menu', '--pick', '0',
                '--no-audio', '--scale', '1', '--no-input',
                '--run-seconds', '4', '--dump-prims', '--log', log]
        subprocess.run(args, timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        out = []
        with open(log, encoding='utf-8', errors='replace') as fp:
            for line in fp:
                m = RE_PRIM.match(line.strip())
                if m:
                    out.append({
                        'id': int(m.group(1)),
                        'type': int(m.group(2)),
                        'draw': int(m.group(3)),
                        'alpha': int(m.group(4)),
                        'parent': int(m.group(5)),
                        'x': int(m.group(6)),
                        'y': int(m.group(7)),
                        'w': int(m.group(8)),
                        'h': int(m.group(9)),
                        'graph': int(m.group(10)),
                    })
        return out
    finally:
        try:
            os.unlink(log)
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--min', type=int, default=100)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not (os.path.isfile(EXE) and os.path.isdir(GAME)):
        print('SKIP: bin/fvp.exe 或游戏目录不存在')
        return 0

    prims = dump_prims()
    print('== 图元树（--dump-prims）')
    check(len(prims) >= a.min, '导出到 ≥%d 个图元（实际 %d）' % (a.min, len(prims)))

    by_id = {p['id']: p for p in prims}
    root = by_id.get(0) if 0 in by_id else None
    if root is not None:
        check(root['parent'] < 0, '0 号图元没有父（parent<0）')
    loose = [p for p in prims if p['id'] != 0 and p['parent'] < 0]
    # `PrimGroupOut(75)` 本来就会把图元从组里摘出来，所以允许个位数；
    # 但绝大多数图元必须挂在根组下（否则 PrimHit 的父链走不到根）。
    check(len(loose) <= 2,
          '除 0 号外"不在任何组里"的图元不超过 2 个（实际 %d: %s）'
          % (len(loose), ','.join(str(p['id']) for p in loose)))
    bad_parent = [p for p in prims if p['id'] != 0 and
                  p['parent'] >= 0 and p['parent'] not in by_id and
                  p['parent'] != 0]
    check(not bad_parent,
          '所有父引用都指向存在的图元（实际 %d 个悬空）' % len(bad_parent))

    print('== 标题背景图元')
    title = [by_id[i] for i in range(250, 262) if i in by_id]
    check(len(title) >= 8, '标题背景/进场图元 250..261 至少 8 个（实际 %d）'
          % len(title))
    check(any(p['draw'] for p in title), '其中至少一个 draw=1')
    check(any(p['alpha'] > 0 for p in title), '其中至少一个 alpha>0')

    if fails:
        print('verify_prim_tree: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_prim_tree: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())