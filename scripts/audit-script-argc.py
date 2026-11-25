#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脚本侧实参审计：用**真实脚本**反查命令表的 argc（不需要跑原版、不需要截图）。

为什么需要它：命令表的 argc 是从 HCB 内嵌元数据里读出来的，读法一旦有偏差，
VM 就会弹错个数的实参（栈失衡），而且这类错误在序章基线里可能根本不触发。
这份脚本换一个独立证据源来对账：**看脚本自己在每个 `syscall` 之前实际压了几个值**。
两套证据（内嵌元数据 vs 实际调用点）互相印证，比任何单一来源可靠。

判定方法（按基本块边界回溯，不做完整的 dataflow）：
  - 从 `syscall` 往前收连续的 push 类指令，直到遇到控制流边界；
  - 若中途遇到"会产生值但不是 push"的指令（call / add / eq / garr_read …），
    该调用点记为 **unknown**（无法静态判定实参个数），不参与对账；
  - 只有能干净回溯到边界的调用点才计入 exact，用它和表里的 argc 比。

产物 `ref/script-argc.json`（只含统计，不含游戏数据）：
  每部作品的 hcb sha256 / 指令数 / exact 与 unknown 站点数 /
  每个命令的 {table_argc, observed, unknown_sites, verdict}、以及 anomalies 列表。

用法：
    python scripts/audit-script-argc.py --game all            # 打印 + 写 ref/script-argc.json
    python scripts/audit-script-argc.py --game all --check    # 只比对，不写文件（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import sys
from collections import Counter, defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': (
        os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
        'iroseka.hcb'),
    'hoshimemo': (
        os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
        'Hoshimemo_HD.hcb'),
}

OUT_JSON = os.path.join(ROOT, 'ref', 'script-argc.json')
CMD_TABLE = os.path.join(ROOT, 'ref', 'cmd_table.json')

PUSHES = {
    'pushint8', 'pushint16', 'pushint32', 'pushfloat', 'pushstring', 'pushtrue',
    'pushfalse', 'pushglobal', 'pushstack', 'pushtop', 'pushtemp',
}
PRODUCERS = {
    'call', 'add', 'sub', 'mul', 'div', 'mod', 'neg', 'eq', 'neq', 'gt', 'le',
    'lt', 'ge', 'test', 'logand', 'logor', 'garr_read', 'larr_read',
    'copystack',
    # syscall 也可能把返回值留在栈上，成为后面某个 syscall 的实参
    # （实测：V3DMotion/V3DSet/SaveThumbSize/CursorMove 的前面就是
    #  `syscall FloatToInt(20)`，返回值就是差掉的那个实参），所以它同样是
    # "会产生值、但个数静态不可数"的边界。
    'syscall',
}
BOUNDARY = {'ret', 'ret2', 'jmp', 'jmpcond', 'initstack', 'nop'}

# TitleMenu 的专门检查：它的实参由 call 的返回值参与构造（见 dump 出来的调用点），
# 所以回溯法会判成 unknown；这里直接断言"紧挨着它的那条指令是一个 push"。
FOCUS_CMDS = {141: 'TitleMenu'}


def audit_game(hcb_path, table):
    with open(hcb_path, 'rb') as fp:
        data = fp.read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)
    exact = defaultdict(Counter)
    unknown = Counter()
    focus = defaultdict(Counter)   # cid -> 紧邻前一条指令的名字

    for i, pc in enumerate(order):
        op, name, args = code.instr[pc]
        if name != 'syscall' or not args:
            continue
        cid = args[0]
        if cid in FOCUS_CMDS and i > 0:
            prev = code.instr[order[i - 1]][1]
            focus[cid][prev] += 1
        k = i - 1
        n = 0
        status = 'exact'
        while k >= 0:
            name2 = code.instr[order[k]][1]
            if name2 in PUSHES:
                n += 1
                k -= 1
                continue
            if name2 in PRODUCERS:
                status = 'unknown'
                break
            if name2 in BOUNDARY:
                break
            status = 'unknown'
            break
        if status == 'exact':
            exact[cid][n] += 1
        else:
            unknown[cid] += 1

    per = {}
    anomalies = []
    for cid in sorted(table):
        e = table[cid]
        obs = exact.get(cid) or Counter()
        item = {
            'name': e['name'],
            'table_argc': e['argc'],
            'observed': {str(k): v for k, v in sorted(obs.items())},
            'unknown_sites': unknown.get(cid, 0),
        }
        if not obs:
            item['verdict'] = 'no-exact-sites'
        elif e['argc'] in obs:
            item['verdict'] = 'match'
        else:
            item['verdict'] = 'mismatch'
            anomalies.append({
                'id': cid,
                'name': e['name'],
                'table_argc': e['argc'],
                'observed': item['observed'],
                'unknown_sites': item['unknown_sites'],
            })
        per[str(cid)] = item

    for cid in sorted(set(exact) | set(unknown)):
        if str(cid) not in per:
            e = table.get(cid, {'name': '?', 'argc': -1})
            per[str(cid)] = {
                'name': e['name'],
                'table_argc': e['argc'],
                'observed': {str(k): v for k, v in sorted(exact[cid].items())},
                'unknown_sites': unknown[cid],
                'verdict': 'not-in-table',
            }

    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'instructions': len(order),
        'exact_sites': sum(sum(c.values()) for c in exact.values()),
        'unknown_sites': sum(unknown.values()),
        'per_command': per,
        'anomalies': anomalies,
        'focus': {
            str(cid): {
                'name': FOCUS_CMDS[cid],
                'prev_instr_hist': dict(sorted(focus[cid].items())),
                'table_argc': table[cid]['argc'] if cid in table else None,
            }
            for cid in FOCUS_CMDS
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    with io.open(CMD_TABLE, encoding='utf-8') as fp:
        table = {e['id']: e for e in json.load(fp)}

    names = sorted(GAMES) if opt.game == 'all' else [opt.game]
    result = {'note': '用真实脚本反查命令表 argc；由 scripts/audit-script-argc.py 生成', 'games': {}}
    for name in names:
        folder, script = GAMES[name]
        path = os.path.join(folder, script)
        if not os.path.isfile(path):
            print('SKIP: %s 不存在（%s）' % (name, path))
            return 0
        r = audit_game(path, table)
        result['games'][name] = r
        print('%s  instructions=%d  exact=%d  unknown=%d  anomalies=%d'
              % (name, r['instructions'], r['exact_sites'], r['unknown_sites'],
                 len(r['anomalies'])))
        for a in r['anomalies']:
            print('   anomaly id=%-4d %-14s table_argc=%-2d observed=%s unknown=%d'
                  % (a['id'], a['name'], a['table_argc'], a['observed'],
                     a['unknown_sites']))
        for cid, f in r['focus'].items():
            print('   focus %s: table_argc=%s prev_instr=%s'
                  % (f['name'], f['table_argc'], f['prev_instr_hist']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 审计结果与 ref/script-argc.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/script-argc.json 一致')
        return 0

    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())