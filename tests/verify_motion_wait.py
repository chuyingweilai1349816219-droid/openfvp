#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题动作等待环门禁：`Motion*Test` 必须能"问完就结束"。

2026-11-27 的根因链：

  * 脚本发 `MotionAlpha(42, 252, 0→255, 1200ms)` 之后紧接
    `while (MotionAlphaTest(252) == 1) yield;`（pc `0x5834A`，实测空转 2 万多次）；
  * 我们的 `Motion*` handler 在"补间引擎关着"（外壳默认）时**只把值落到位、不登记
    记录** → `Motion*Test` 恒答 0 → 等待环永远出不来；
  * 修法：关着补间时**仍然登记记录**（值照旧直接落位），并让 `fvp_motion_test`
    只在"活跃（没跑完）"时答 1 —— 对应原版 `0x422990` 查的 `+0x4` 活跃标志。

门禁用 `FVP_TRACE_CIDS=1`（每条命令首次出现的步数）看"命令覆盖"：
修之前窗口只到 **60** 条、最后一条是 `44`；修之后能到 **≥63** 条，并在 `44 之后`
继续出现 `4 / 0 / 1`（越过等待环之后的命令）。
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
EXE = os.path.join(FVP, 'bin', 'fvp.exe')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
RE_CID = re.compile(r'^cid (\d+) first at step (\d+)$')


def trace_cids(seconds=15):
    env = dict(os.environ)
    env['FVP_TRACE_CIDS'] = '1'
    args = [EXE, '--game', GAME, '--title-menu', '--pick', '0',
            '--no-audio', '--scale', '1', '--no-input',
            '--run-seconds', str(seconds)]
    proc = subprocess.run(args, timeout=180 + seconds, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = []
    for raw in proc.stdout.decode('utf-8', 'replace').splitlines():
        m = RE_CID.match(raw.strip())
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--min-cids', type=int, default=63)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not (os.path.isfile(EXE) and os.path.isdir(GAME)):
        print('SKIP: bin/fvp.exe 或游戏目录不存在')
        return 0

    cids = trace_cids()
    ids = [c for c, _ in cids]
    print('== 标题阶段命令覆盖（FVP_TRACE_CIDS）')
    check(len(ids) >= a.min_cids,
          '命令覆盖 ≥%d 条（实际 %d）' % (a.min_cids, len(ids)))
    check(44 in ids, '`44 MotionAlphaTest` 出现过（等待环确实被走到）')
    if 44 in ids:
        after = [c for c in ids[ids.index(44) + 1:]]
        check(len(after) >= 1,
              '`44` 之后还有新命令（实际 %d 条：%s）'
              % (len(after), ','.join(str(x) for x in after[:6])))
        check(any(c in (0, 1, 4) for c in after),
              '`44` 之后出现 0/1/4（越过等待环之后才有的命令）')

    if fails:
        print('verify_motion_wait: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_motion_wait: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())