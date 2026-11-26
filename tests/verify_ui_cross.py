#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""跨作品 UI 素材对账门禁（ref/ui-cross.json）。

主张：两部作品的游戏内 UI 用的是**同一套素材命名与图元号**，差别只有三处：

  1. `sys_`：五彩斑斓 44 条、星空的记忆 35 条 —— 少的 9 条正好是 8 张 `_M` 遮罩
     加 `sys_window_title`（即五彩的环形菜单有第三态与标题条，星空只有两态）；
  2. `slf_`：星空多一张 `slf_prt_mask`；
  3. `menu_` 与 `fc_`：两作**完全相同**（各 14 / 37 条）。

有游戏数据时重新扫描逐字段比对；缺失时只校验冻结 JSON（不伪造通过）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'ui-cross.json')
SCANNER = os.path.join(FVP, 'scripts', 'scan-ui-cross.py')

RING = ['save', 'load', 'hidewin', 'undo', 'config', 'title', 'exit', 'return']
IRO_SYS_ONLY = ['sys_config_M', 'sys_exit_M', 'sys_hidewin_M', 'sys_load_M',
                'sys_return_M', 'sys_save_M', 'sys_title_M', 'sys_undo_M',
                'sys_window_title']


def load_scanner():
    spec = importlib.util.spec_from_file_location('scan_ui_cross', SCANNER)
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
        print('verify_ui_cross: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-ui-cross/1', 'schema fvp-ui-cross/1')
    games = ref.get('games', {})
    check(sorted(games) == ['hoshimemo', 'iroseka'], '两作都在基线里')
    loads = {g: len(v['loads']) for g, v in games.items()}
    check(loads.get('iroseka') == 98 and loads.get('hoshimemo') == 90,
          '载入点数 98 / 90（实际 %s）' % loads)

    fam = ref.get('families', {})
    counts = {g: {k: len(v) for k, v in fam[g].items()} for g in fam}
    check(counts['iroseka'] == {'sys': 44, 'menu': 14, 'fc': 37, 'slf': 3},
          '五彩家族计数 44/14/37/3（实际 %s）' % counts['iroseka'])
    check(counts['hoshimemo'] == {'sys': 35, 'menu': 14, 'fc': 37, 'slf': 4},
          '星空家族计数 35/14/37/4（实际 %s）' % counts['hoshimemo'])

    print('== 差异只有三处')
    iro = {k: set(v) for k, v in fam['iroseka'].items()}
    hos = {k: set(v) for k, v in fam['hoshimemo'].items()}
    check(sorted(iro['sys'] - hos['sys']) == IRO_SYS_ONLY,
          '五彩多出的 9 条 = 8 张 _M + sys_window_title')
    check(not (hos['sys'] - iro['sys']), '星空没有五彩缺的 sys_ 素材')
    check(sorted(hos['slf'] - iro['slf']) == ['slf_prt_mask'],
          '星空多一张 slf_prt_mask')
    check(iro['menu'] == hos['menu'], 'menu_ 两作完全相同（14 条）')
    check(iro['fc'] == hos['fc'], 'fc_ 两作完全相同（37 条）')

    print('== 环形菜单的 8 个动作（两作都要有两态）')
    for game in ('iroseka', 'hoshimemo'):
        missing = [x for act in RING for x in ('sys_%s_1' % act, 'sys_%s_2' % act)
                   if x not in fam[game]['sys']]
        check(not missing, '%s: 8 动作 × 2 态齐备（缺 %s）' % (game, missing or '无'))
    check(all('sys_%s_M' % act in iro['sys'] for act in RING),
          '五彩：8 动作各有 _M 第三态')
    check(not any(x.endswith('_M') and x.startswith('sys_') for x in hos['sys']),
          '星空：没有任何 sys_*_M')

    print('== 重新扫描对账')
    mod = load_scanner()
    got = mod.build()
    if got is None:
        print('  skip  游戏数据缺失')
    else:
        check(sorted(got['games']) == sorted(games), '参与对账的作品一致')
        for game in games:
            if game not in got['games']:
                continue
            check(got['games'][game]['loads'] == games[game]['loads'],
                  '%s：载入点逐条相等' % game)
            check(got['games'][game]['sha256'] == games[game]['sha256'],
                  '%s：hcb sha256 不变' % game)
        check(got['families'] == fam, '家族清单相等')

    if fails:
        print('verify_ui_cross: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_ui_cross: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())