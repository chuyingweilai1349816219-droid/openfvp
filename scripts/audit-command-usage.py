#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""两部作品**实际用到**哪些命令、我们的引擎是否都实现了（多作品兼容性矩阵的一环）。

为什么：命令表有 148 条，但每部作品真正调用的是一部分。想知道"我们的引擎对这两部
作品够不够用"，最直接的证据是**脚本里出现过的 syscall id 集合**，和 `src/syscall.c`
里真正实现的 `case` 集合做差。

产物 `ref/command-usage.json`（只含统计，不含游戏数据）：
每部作品：hcb sha256 / 总指令数 / syscall 总数 / 用到的 id 数 / 每个 id 的
{name, count, handler, handled}；以及两张缺失清单。

用法：
    python scripts/audit-command-usage.py            # 打印 + 写 json
    python scripts/audit-command-usage.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys
from collections import Counter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': (os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
                'iroseka.hcb'),
    'hoshimemo': (os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
                  'Hoshimemo_HD.hcb'),
}
OUT_JSON = os.path.join(ROOT, 'ref', 'command-usage.json')
CMD_TABLE = os.path.join(ROOT, 'ref', 'cmd_table.json')
DISPATCH = os.path.join(ROOT, 'ref', 'exe-cmd-dispatch.json')
SYSCALL_C = os.path.join(ROOT, 'src', 'syscall.c')


def handled_ids():
    src = io.open(SYSCALL_C, encoding='utf-8', errors='replace').read()
    return sorted(set(int(m.group(1)) for m in
                      re.finditer(r'case\s+(\d+)\s*:', src)))


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(SYSCALL_C):
        print('SKIP: 缺 src/syscall.c')
        return 0
    table = {c['id']: c for c in
             json.load(io.open(CMD_TABLE, encoding='utf-8'))}
    dispatch = {}
    if os.path.isfile(DISPATCH):
        d = json.load(io.open(DISPATCH, encoding='utf-8'))
        dispatch = {c['id']: c['handler'] for c in d.get('commands') or []}
    have = set(handled_ids())

    games = {}
    missing_all = {}
    for name, (d, fn) in GAMES.items():
        path = os.path.join(d, fn)
        if not os.path.isfile(path):
            print('SKIP: %s 不存在' % path)
            return 0
        data = io.open(path, 'rb').read()
        code = fvm_ref.Code(data, 'cp932')
        used = Counter()
        total_instr = len(code.instr)
        for pc, (op, opname, args) in code.instr.items():
            if opname == 'syscall' and args:
                used[int(args[0])] += 1
        entries = []
        missing = []
        for cid in sorted(used):
            handled = cid in have
            entries.append({
                'id': cid,
                'name': (table.get(cid) or {}).get('name', '?'),
                'count': used[cid],
                'handler': dispatch.get(cid, '?'),
                'handled': handled,
            })
            if not handled:
                missing.append(cid)
        games[name] = {
            'hcb_sha256': hashlib.sha256(data).hexdigest(),
            'instructions': total_instr,
            'syscalls': sum(used.values()),
            'distinct_ids': len(used),
            'commands': entries,
            'missing_in_engine': missing,
        }
        missing_all[name] = missing

    result = {
        'note': '两部作品实际用到的 syscall id 与引擎实现情况的对照（A 类统计）；'
                '由 scripts/audit-command-usage.py 生成',
        'engine_cases': len(have),
        'games': games,
        'summary': {name: {'distinct_ids': g['distinct_ids'],
                           'missing': len(missing_all[name])}
                    for name, g in games.items()},
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    for name, g in games.items():
        print('== %s：指令 %d，syscall %d，用到 %d 个 id，未实现 %d =='
              % (name, g['instructions'], g['syscalls'], g['distinct_ids'],
                 len(g['missing_in_engine'])))
        if g['missing_in_engine']:
            print('  缺: ' + ', '.join(
                '%d:%s' % (e['id'], e['name']) for e in g['commands']
                if not e['handled']))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        if io.open(OUT_JSON, encoding='utf-8').read() != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())