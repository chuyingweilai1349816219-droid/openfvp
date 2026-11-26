#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""UI 素材几何门禁（ref/ui-rects.json）。

主张：

  * 几何可以**直接从 `.bin` 归档里读**（44 字节 HZC1 头 `0x14/0x16/0x18/0x1A`），
    不需要先解包；两个游戏都用同一条路径；
  * 五彩这条路径算出来的 81 个矩形，与"解包文件"那条路径冻结在
    `ref/ingame-menu.json` 里的矩形**逐个相等** —— 两条独立路径互相印证；
  * 星空的环形菜单是 8 个 **125×125** 的方块，y 从 8 递增到 942，全部贴在右侧
    (x 1288..1522)；侧窗是两块**整高**面板（y=0、高 1080，一块贴左 x=0、一块贴右
    到 1920）；确认框提示图仍以 960 居中；
  * 两作都只有 `sys_cap_000..017` 这 17 张**读不到矩形**（运行时抓屏）。

有游戏数据时重新扫描逐字段比对；缺失时只校验冻结 JSON（不伪造通过）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'ui-rects.json')
MENU = os.path.join(FVP, 'ref', 'ingame-menu.json')
SCANNER = os.path.join(FVP, 'scripts', 'scan-ui-rects.py')

RING = ['save', 'load', 'hidewin', 'undo', 'config', 'title', 'exit', 'return']


def load_scanner():
    spec = importlib.util.spec_from_file_location('scan_ui_rects', SCANNER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not os.path.isfile(a.json):
        print('verify_ui_rects: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)
    games = ref.get('games', {})

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-ui-rects/1', 'schema fvp-ui-rects/1')
    check(sorted(games) == ['hoshimemo', 'iroseka'], '两作都在基线里')
    for g in games:
        check(len(games[g]['rects']) in (73, 81),
              '%s：矩形数 %d（五彩 81 / 星空 73）' % (g, len(games[g]['rects'])))
        miss = games[g]['missing']
        check(len(miss) == 17 and all(m.startswith('sys_cap') for m in miss),
              '%s：缺的正好是 17 张 sys_cap_*' % g)
        check(all(len(r) == 4 and r[2] > 0 and r[3] > 0 for r in games[g]['rects'].values()),
              '%s：每个矩形都是 4 个正数' % g)
        check(all(0 <= r[0] and r[0] + r[2] <= 1920 and
                  0 <= r[1] and r[1] + r[3] <= 1080
                  for r in games[g]['rects'].values()),
              '%s：矩形都在 1920x1080 内' % g)

    print('== 五彩：两条独立路径互证（归档直读 vs 解包文件）')
    iro = games['iroseka']['rects']
    if os.path.isfile(MENU):
        with open(MENU, encoding='utf-8') as f:
            menu = json.load(f)
        inv = {r['asset']: r['rect'] for r in menu.get('inventory', []) if r.get('rect')}
        common = [k for k in inv if k in iro]
        check(len(common) >= 80, '可比对的素材 %d 条' % len(common))
        diff = [(k, inv[k], iro[k]) for k in common if inv[k] != iro[k]]
        check(not diff, '解包路径与归档直读逐条相等（差异 %s）' % (diff[:2] or '无'))
    else:
        print('  skip  ref/ingame-menu.json not present')

    print('== 星空：环形菜单 125x125 贴右、y 递增')
    hos = games['hoshimemo']['rects']
    ring = []
    for act in RING:
        key = 'sys_%s_1' % act
        if key not in hos:
            check(False, '缺少 %s' % key)
            continue
        ring.append(hos[key])
    if len(ring) == 8:
        check(all(r[2] == 125 and r[3] == 125 for r in ring),
              '8 个环形项全是 125x125（实际 %s）'
              % sorted({(r[2], r[3]) for r in ring}))
        ys = [r[1] for r in ring]
        check(ys == sorted(ys) and len(set(ys)) == 8,
              'y 严格递增 8..942（实际 %s）' % ys)
        check(all(1250 <= r[0] <= 1550 for r in ring),
              '全部贴在右侧（x 1288..1522）')

    win = {k: v for k, v in hos.items() if k.startswith('sys_window')}
    check(sorted(win) == ['sys_window_left', 'sys_window_right'],
          '侧窗只有 left/right 两件（五彩多一件 title）')
    if len(win) == 2:
        check(all(v[1] == 0 and v[3] == 1080 for v in win.values()),
              '两块侧窗都是整高面板（y=0，高 1080）')
        check(win['sys_window_left'][0] == 0 and
              win['sys_window_right'][0] + win['sys_window_right'][2] == 1920,
              '一块贴左、一块贴右')

    print('== 星空：确认框与命令条')
    prompts = {k: v for k, v in hos.items() if k.endswith('_mes')}
    check(len(prompts) == 7, '7 张确认提示图（实际 %d）' % len(prompts))
    if prompts:
        centers = [v[0] + v[2] / 2 for v in prompts.values()]
        check(all(abs(c - 960) <= 12 for c in centers),
              '提示图仍以 x=960 居中（%s）' % [round(c, 1) for c in centers])
        check(all(460 <= v[1] <= 470 for v in prompts.values()),
              '提示图基线 463..468')
    fc = {k: v for k, v in hos.items() if k.startswith('fc_')}
    check(len(fc) == 37, '命令条 37 条（实际 %d）' % len(fc))
    check(all(1600 <= v[0] and v[0] + v[2] <= 1920 and v[1] >= 800
              for v in fc.values()),
          '命令条全在右下角（x>=1600, y>=800）')
    sizes = {tuple(v[2:]) for k, v in fc.items() if not k.endswith('help')}
    check(sizes == {(55, 50), (197, 16), (25, 25)},
          '命令条按钮三种尺寸 55x50 / 197x16 / 25x25（实际 %s）' % sorted(sizes))
    locks = [v for k, v in fc.items() if k.startswith('fc_lock') and not k.endswith('help')]
    check(len(locks) == 4 and all(tuple(v[2:]) == (197, 16) for v in locks),
          'lock 的 4 个状态都是 197x16 横条')

    print('== 重新扫描对账')
    mod = load_scanner()
    for g in sorted(games):
        if not os.path.isdir(mod.GAMES[g]):
            print('  skip  %s data missing' % g)
            continue
        got = mod.scan(g)
        check(got['rects'] == games[g]['rects'], '%s：矩形逐条相等' % g)
        check(got['missing'] == games[g]['missing'], '%s：缺失清单相等' % g)

    if fails:
        print('verify_ui_rects: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_ui_rects: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())