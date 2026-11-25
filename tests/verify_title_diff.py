#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题画面"误差分布"门禁：总平均差要拆到区域上，且数字必须与基线一致。

为什么：总平均差 13.11 是个标量，看不出下一步修哪里。`scripts/analyze-title-diff.py`
按**素材头矩形**把画面切成 logo / menu / copyright / rest 四块，分别算平均差。
本门禁：

  1. 重新分析的结果与 `ref/title-diff-regions.json` **逐字节一致**
     （渲染或工具逻辑漂移立刻红）；
  2. 总平均差 == `tests/original-baseline.json` 里冻结的 iroseka 值（±0.05）；
  3. **误差最大的区域必须是 copyright**（2026-11-25 的实测结论），
     一旦某个区域修好了、排名变化，本断言会显式失败，逼着更新基线而不是悄悄漂。

区域矩形来自素材头（logo 1150x283@(701,149)、copyright 590x37@(1189,1043)、
menu = 5 个菜单项并集），不是手画框。

缺游戏目录/原版帧/Pillow 时 SKIP 并返回 0。

用法（用 DSH 运行时 Python，需要 Pillow+numpy）：
    python tests/verify_title_diff.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
ANALYZE = os.path.join(FVP, 'scripts', 'analyze-title-diff.py')
REF = os.path.join(FVP, 'ref', 'title-diff-regions.json')
BASELINE = os.path.join(HERE, 'original-baseline.json')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    try:
        import numpy  # noqa: F401
        from PIL import Image  # noqa: F401
    except ImportError:
        print('SKIP: 缺 Pillow/numpy（这是可选门禁）')
        return 0
    if not os.path.isdir(GAME) or not os.path.isfile(os.path.join(
            FVP, 'out', 'orig-title', 'orig-title-005.png')):
        print('SKIP: 缺游戏目录或原版标题帧')
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新分析并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', ANALYZE, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'analyze-title-diff.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    with io.open(BASELINE, encoding='utf-8') as fp:
        cases = {c['name']: c for c in json.load(fp)['cases']}

    print('== 2. 总平均差与冻结基线一致 ==')
    total = (ref.get('total') or {}).get('mean_abs')
    want = cases['iroseka-title']['baseline']['mean_abs']
    check(total is not None and abs(total - want) <= 0.05,
          '总平均差 %.4f ~ 基线 %.2f（±0.05）' % (total or -1, want))

    print('== 3. 误差最大的区域（按平均差） ==')
    regions = ref.get('regions') or {}
    scored = [(v['mean_abs'], k) for k, v in regions.items()
              if v.get('mean_abs') is not None]
    scored.sort(reverse=True)
    for mean_abs, name in scored:
        print('   %-10s mean_abs=%.2f' % (name, mean_abs))
    check(scored and scored[0][1] == 'copyright',
          '误差最大的区域是 copyright（实际 %s）'
          % (scored[0][1] if scored else None))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题画面误差分布与基线一致（copyright 仍是最大头）')
    return 0


if __name__ == '__main__':
    sys.exit(main())