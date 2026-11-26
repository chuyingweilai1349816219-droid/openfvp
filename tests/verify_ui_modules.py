#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""UI 模块地址门禁（ref/ui-modules.json）。

主张：两作的 UI 在脚本里是**同构的两个模块**，只是偏移不同：

| 模块 | 五彩斑斓 | 星空的记忆 | 共同点 |
|---|---|---|---|
| 环形菜单装载器 | `0x54520`（481 指令 / 44 素材） | `0x6B8E1`（401 / 35） | 图元 141..421、**只有 1 个调用点** |
| 确认框装载器 | `0x57383`（345 / 13） | `0x6E297`（324 / 13） | 图元 93..137、**只有 1 个调用点** |
| 入口函数 | `0x54C68` / `0x577C8` | `0x6BEE4` / `0x6E6BA` | 各由装载器调用点反推 |

有游戏数据时重新扫描逐字段比对；缺失时只校验冻结 JSON（不伪造通过）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'ui-modules.json')
SCANNER = os.path.join(FVP, 'scripts', 'scan-ui-modules.py')

EXPECT = {
    'iroseka': {
        'ring': dict(fn='0x54520', instructions=481, assets=44,
                     prim_min=141, prim_max=421, entry='0x54c68'),
        'dialog': dict(fn='0x57383', instructions=345, assets=13,
                       prim_min=93, prim_max=137, entry='0x577c8'),
    },
    'hoshimemo': {
        'ring': dict(fn='0x6b8e1', instructions=401, assets=35,
                     prim_min=141, prim_max=421, entry='0x6bee4'),
        'dialog': dict(fn='0x6e297', instructions=324, assets=13,
                       prim_min=93, prim_max=137, entry='0x6e6ba'),
    },
}


def load_scanner():
    spec = importlib.util.spec_from_file_location('scan_ui_modules', SCANNER)
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
        print('verify_ui_modules: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)
    games = ref.get('games', {})

    check(ref.get('schema') == 'fvp-ui-modules/1', 'schema fvp-ui-modules/1')
    check(sorted(games) == ['hoshimemo', 'iroseka'], '两作都在基线里')

    print('== 每个模块的地址 / 规模 / 调用点')
    for game in ('iroseka', 'hoshimemo'):
        g = games.get(game, {})
        for key in ('ring', 'dialog'):
            m = g.get(key, {})
            want = EXPECT[game][key]
            for field in ('fn', 'instructions', 'assets', 'prim_min',
                          'prim_max', 'entry'):
                check(m.get(field) == want[field],
                      '%s %s %s = %s（实际 %s）'
                      % (game, key, field, want[field], m.get(field)))
            check(len(m.get('callers', [])) == 1,
                  '%s %s 只有 1 个调用点（实际 %d）'
                  % (game, key, len(m.get('callers', []))))

    print('== 两作同构：图元范围完全一致')
    for key in ('ring', 'dialog'):
        spans = {(games[g][key]['prim_min'], games[g][key]['prim_max'])
                 for g in games}
        check(len(spans) == 1, '%s 的图元范围两作相同（%s）' % (key, spans))

    print('== 装载器只有一个调用点 -> 入口函数可反推')
    for game in games:
        for key in ('ring', 'dialog'):
            m = games[game][key]
            check(m.get('entry') is not None,
                  '%s %s 能反推出入口函数 %s' % (game, key, m.get('entry')))

    print('== 重新扫描对账')
    mod = load_scanner()
    got = mod.build()
    if got is None:
        print('  skip  游戏数据缺失')
    else:
        check(got['games'] == games, '两作模块表逐字段相等')

    if fails:
        print('verify_ui_modules: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_ui_modules: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())