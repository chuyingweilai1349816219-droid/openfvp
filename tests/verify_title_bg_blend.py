#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题背景合成模型的门禁：18 个候选里必须仍是"measured 顺序 + over"最好。

为什么：背景四层（A/B/C/D）是带 alpha 的 BGRA 图，合成模型（顺序 × 混合 × 底色）
直接决定标题画面背景的对不对。`scripts/analyze-title-bg-blend.py` 在 numpy 里把
18 个候选都算一遍，拿**原版标题帧的纯背景区域**（扣掉 logo/菜单/版权行）比平均差，
本门禁把结论冻结：

  1. `--check` 与 `ref/title-bg-blend.json` **逐字节一致**；
  2. 最好的候选必须是 `order=measured, blend=over`（即我们渲染器的做法：
     顺序 A→C→D→B、直通 alpha 的 over 合成）；
  3. 按 Z 的两种自然顺序都必须**比它差**（z-ascending / z-descending），
     否则说明我们对 Z 的理解变了，需要重新讨论而不是悄悄改；
  4. 最好候选的平均差落在 [8, 15] 区间（2026-11-25 实测 11.56）——
     太小说明素材被换了、太大说明合成模型错了。

缺 Pillow/numpy/游戏目录/原版帧时 SKIP 并返回 0。

用法（DSH 运行时 Python）：
    python tests/verify_title_bg_blend.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
ANALYZE = os.path.join(FVP, 'scripts', 'analyze-title-bg-blend.py')
REF = os.path.join(FVP, 'ref', 'title-bg-blend.json')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
FRAME = os.path.join(FVP, 'out', 'orig-title', 'orig-title-005.png')

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
        print('SKIP: 缺 Pillow/numpy（可选门禁）')
        return 0
    if not os.path.isdir(GAME) or not os.path.isfile(FRAME):
        print('SKIP: 缺游戏目录或原版标题帧')
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新扫描并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', ANALYZE, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'analyze-title-bg-blend.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    best = ref.get('best') or {}
    cands = ref.get('candidates') or []

    print('== 2. 最优候选必须是 measured + over ==')
    check(best.get('order') == 'measured' and best.get('blend') == 'over',
          '最优候选 = measured/over（实际 %s/%s）'
          % (best.get('order'), best.get('blend')))
    m = best.get('mean_abs')
    check(m is not None and 8.0 <= m <= 15.0,
          '最优候选平均差在 [8,15]（实际 %r）' % (m,))

    print('== 3. 按 Z 的两种自然顺序都必须更差 ==')
    def find(order, blend):
        for c in cands:
            if c['order'] == order and c['blend'] == blend:
                return c['mean_abs']
        return None

    for order in ('z-ascending', 'z-descending'):
        other = find(order, 'over')
        check(other is not None and m is not None and other > m,
              '%s/over 平均差 %.3f 必须大于 measured/over %.3f'
              % (order, other if other is not None else -1, m if m else -1))

    print('== 3.5 缩放：原版帧处在缩放动画的**终点**（1.0），不是 0.9/0.8 ==')
    def find_scale(scale_name, blend='over'):
        for c in cands:
            if (c.get('scales') == scale_name and c['blend'] == blend
                    and c['order'] == 'measured'):
                return c['mean_abs']
        return None

    check(best.get('scales') == 'all-1.0',
          '最优候选的缩放是 all-1.0（实际 %r）' % (best.get('scales'),))
    for tag in ('final-900/800', 'from-1300/1600/2500/4000'):
        other = find_scale(tag)
        check(other is not None and m is not None and other > m,
              '%s 的平均差 %.3f 必须大于 all-1.0 的 %.3f'
              % (tag, other if other is not None else -1, m if m else -1))

    print('== 4. 预乘/加色都不如 over ==')
    for blend in ('premul', 'add'):
        other = find('measured', blend)
        check(other is not None and m is not None and other >= m,
              'measured/%s 平均差 %.3f 不优于 over %.3f'
              % (blend, other if other is not None else -1, m if m else -1))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 背景合成模型 = measured 顺序 + 直通 alpha over（已用原版帧验证）')
    return 0


if __name__ == '__main__':
    sys.exit(main())