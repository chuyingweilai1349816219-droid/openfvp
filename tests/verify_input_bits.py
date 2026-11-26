#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""输入掩码使用表门禁（ref/input-bits.json）。

主张：脚本对输入只测很窄的一组掩码，而且两作几乎一样。术语统一成"掩码值（第 n 位）"，
避免历史上 "bit1 = 第 0 位 / 第 1 位" 的混用。

| 掩码值 | 第几位 | 五彩 | 星空 |
|---|---|---|---|
| 0 | —（只读状态） | 32 | 32 |
| 1 | 0 | 115 | 113 |
| 2 | 1 | 23 | 20 |
| 3 | 0+1 | 34 | 30 |
| 4 | 2 | 6 | 6 |
| 5 | 0+2 | 2 | 2 |
| 6 | 1+2 | 1 | 1 |
| 7 | 0+1+2 | 2 | 2 |
| 8 | 3 | 6 | 3 |
| 10 | 1+3 | 5 | 4 |
| 16 | 4 | 1 | 1 |

换算到"第 n 位被用到多少次"：五彩 `153/65/11/11/1`、星空 `147/57/11/7/1`（第 0..4 位）。
环形菜单那 4 个输入点的掩码正是 `1`（第 0 位）。

有游戏数据时重新扫描逐字段比对；缺失时只校验冻结 JSON（不伪造通过）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'input-bits.json')
MODULES = os.path.join(FVP, 'ref', 'ui-modules.json')
SCANNER = os.path.join(FVP, 'scripts', 'scan-input-bits.py')

MASKS = {'iroseka': {'0': 32, '1': 115, '2': 23, '3': 34, '4': 6, '5': 2,
                     '6': 1, '7': 2, '8': 6, '10': 5, '16': 1, 'none': 11},
         'hoshimemo': {'0': 32, '1': 113, '2': 20, '3': 30, '4': 6, '5': 2,
                       '6': 1, '7': 2, '8': 3, '10': 4, '16': 1, 'none': 11}}
BITS = {'iroseka': {'0': 153, '1': 65, '2': 11, '3': 11, '4': 1},
        'hoshimemo': {'0': 147, '1': 57, '2': 11, '3': 7, '4': 1}}
CALLS = {'iroseka': {'InputGetState': 91, 'InputGetDown': 147},
         'hoshimemo': {'InputGetState': 82, 'InputGetDown': 143}}


def load_scanner():
    spec = importlib.util.spec_from_file_location('scan_input_bits', SCANNER)
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
        print('verify_input_bits: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)
    games = ref.get('games', {})

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-input-bits/1', 'schema fvp-input-bits/1')
    check(sorted(games) == ['hoshimemo', 'iroseka'], '两作都在基线里')
    check('第 0 位' in ref.get('note', '') or 'bit0' in ref.get('note', ''),
          'note 里写明 mask=1 是第 0 位')

    print('== 掩码计数（脚本实际测的值）')
    for game in ('iroseka', 'hoshimemo'):
        g = games.get(game, {})
        check(g.get('masks') == MASKS[game],
              '%s: 11 个掩码值计数与基线一致' % game)
        check(g.get('calls') == CALLS[game],
              '%s: 调用计数 %s' % (game, CALLS[game]))
        check(g.get('bits') == BITS[game],
              '%s: 位统计 %s（第 0..4 位）' % (game, BITS[game]))

    print('== 两作一致性与"最常用的位"')
    check(sorted(games['iroseka']['masks']) == sorted(games['hoshimemo']['masks']),
          '两作用的掩码值集合完全相同')
    check(all(int(k) < 32 for k in games['iroseka']['masks'] if k != 'none'),
          '只用到第 0..4 位（掩码值 < 32）')
    for game in ('iroseka', 'hoshimemo'):
        b = games[game]['bits']
        top = max(b, key=lambda k: b[k])
        check(top == '0', '%s: 最常用的是第 0 位（%d 次）' % (game, b[top]))
        check(b.get('4') == 1, '%s: 第 4 位只被用 1 次' % game)

    print('== 与环形菜单的打开路径对得上')
    if os.path.isfile(MODULES):
        with open(MODULES, encoding='utf-8') as f:
            mod = json.load(f)
        for game in ('iroseka', 'hoshimemo'):
            driven = [o for o in mod['games'][game]['ring']['openers']
                      if o.get('input_driven')]
            check(all(o['masks'] == [1] for o in driven),
                  '%s: 环形菜单 4 个输入点的掩码都是 1' % game)
            check('1' in games[game]['masks'],
                  '%s: 掩码 1 出现在输入使用表里' % game)
    else:
        print('  skip  ref/ui-modules.json not present')

    print('== 重新扫描对账')
    mod_scan = load_scanner()
    got = mod_scan.build()
    if got is None:
        print('  skip  游戏数据缺失')
    else:
        check(got['games'] == games, '两作掩码表逐字段相等')

    if fails:
        print('verify_input_bits: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_input_bits: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())