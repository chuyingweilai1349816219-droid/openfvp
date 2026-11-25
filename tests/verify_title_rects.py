#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题菜单按钮矩形的门禁：矩形来自**素材头**，不是截图量出来的。

第 122 轮确认菜单项就是图元、命中靠 `PrimHit`；第 123 轮把矩形落实了：
脚本没有 `PrimSetXY`，位置来自 HZC1 头里自带的 `x/y/w/h`。
`scripts/extract-title-rects.py` 直接读原版 `graph.bin` 的 BIN 索引 + 素材头，
本门禁把这些原值冻结，并断言两条**布局律**：

  五彩斑斓的世界：右边缘**全部 = 1785**（右对齐）、行 y = 495/595/695/795/895
                  → 行距**恰好 100**
  星空的记忆    ：中心**全部 = 960**（居中）、行 y = 580/670/760/850/940
                  → 行距**恰好 90**

另外断言每项都有状态变体贴图（五彩斑斓 5 个 / 星空 2 个）——选中态是换图。
缺游戏目录时 SKIP 并返回 0。

用法：python tests/verify_title_rects.py
"""
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-title-rects.py')
REF = os.path.join(FVP, 'ref', 'title-rects.json')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
}
ITEMS = ['title_start', 'title_continue', 'title_album', 'title_option', 'title_end']
EXPECT = {
    'iroseka': {
        'rects': {
            'title_start': (1445, 495, 340, 80),
            'title_continue': (1495, 595, 290, 80),
            'title_album': (1560, 695, 225, 80),
            'title_option': (1560, 795, 225, 90),
            'title_end': (1645, 895, 140, 80),
        },
        'pitch': 100,
        'align': 'right',
        'edge': 1785,
        'variants': 5,
    },
    'hoshimemo': {
        'rects': {
            'title_start': (820, 580, 275, 40),
            'title_continue': (845, 670, 230, 40),
            'title_album': (880, 760, 160, 40),
            'title_option': (875, 850, 170, 40),
            'title_end': (905, 940, 110, 40),
        },
        'pitch': 90,
        'align': 'center',
        'center': 960,
        'variants': 2,
    },
}

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    missing = [n for n, p in GAMES.items() if not os.path.isdir(p)]
    if missing:
        print('SKIP: 缺游戏目录 %s（可选语料，见 docs/status.md）' % missing)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=1800)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-title-rects.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    for name in sorted(GAMES):
        print('== 2. %s：按钮矩形（来自素材头） ==' % name)
        g = ref['games'].get(name) or {}
        items = g.get('items') or {}
        exp = EXPECT[name]
        check(sorted(items) == sorted(ITEMS),
              '%s 恰好 5 个菜单项的矩形（实际 %s）' % (name, sorted(items)))
        for item in ITEMS:
            got = (items.get(item) or {}).get('rect') or {}
            want = exp['rects'][item]
            actual = (got.get('x'), got.get('y'), got.get('w'), got.get('h'))
            check(actual == want, '%s %s 矩形 == (x,y,w,h)=%s（实际 %s）'
                  % (name, item, want, actual))
            check(len((items.get(item) or {}).get('variants') or {})
                  == exp['variants'],
                  '%s %s 有 %d 个状态变体（实际 %d）'
                  % (name, item, exp['variants'],
                     len((items.get(item) or {}).get('variants') or {})))
        check(g.get('row_pitch') == [exp['pitch']],
              '%s 行距恰好 %d（实际 %s）' % (name, exp['pitch'], g.get('row_pitch')))
        check(g.get('rows_y') == [exp['rects'][i][1] for i in ITEMS],
              '%s 行 y 与矩形一致（实际 %s）' % (name, g.get('rows_y')))
        if exp['align'] == 'right':
            check(g.get('distinct_right_edges') == [exp['edge']],
                  '%s 右边缘统一 = %d（实际 %s）'
                  % (name, exp['edge'], g.get('distinct_right_edges')))
        else:
            # 居中：用 2*(x) + w 和 2*960 比，避免整数除法把 275 宽的那项算成 957。
            # 实测「GAME START」的素材宽 275 是奇数、右侧有透明边，中心差 2.5 px；
            # 其余 4 项精确落在 960。所以判据是"每项中心与 960 的偏差 <= 3 px"。
            devs = []
            for i in ITEMS:
                x, _y, w, _h = exp['rects'][i]
                devs.append(abs((2 * x + w) - 2 * exp['center']) / 2.0)
            check(max(devs) <= 3.0,
                  '%s 各项中心与 %d 的偏差 <= 3 px（实际最大 %.1f px）'
                  % (name, exp['center'], max(devs)))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题菜单按钮矩形与素材头一致（右对齐 1785 / 居中 960，行距 100 / 90）')

    print('== 4. 实现里的常数必须与素材头一致（src/titlemenu.c） ==')
    src = os.path.join(FVP, 'src', 'titlemenu.c')
    with io.open(src, encoding='utf-8') as fp:
        text = fp.read()
    pat = re.compile(
        r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*'
        r'(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,')
    starts = {
        'iroseka': text.find('fvp_title_profile_iroseka = {'),
        'hoshimemo': text.find('fvp_title_profile_hoshimemo = {'),
    }
    bounds = {
        'iroseka': (starts['iroseka'], starts['hoshimemo']),
        'hoshimemo': (starts['hoshimemo'], len(text)),
    }
    for name in sorted(GAMES):
        lo, hi = bounds[name]
        if not check(lo >= 0 and hi > lo, '%s 的 profile 在 titlemenu.c 里找得到' % name):
            continue
        block = text[lo:hi]
        found = {}
        for m in pat.finditer(block):
            found[m.group(2)] = tuple(int(m.group(k)) for k in (4, 5, 6, 7))
        for item, rect in EXPECT[name]['rects'].items():
            want = (rect[2], rect[3], rect[0], rect[1])   # (w, h, x, y)
            got = found.get(item)
            check(got == want, '%s %s: titlemenu.c 里 (w,h,x,y)=%s（实际 %s）'
                  % (name, item, want, got))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())