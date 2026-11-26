#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract-exe-text-layout.py -- 把「文本排版笔位」这条链从 exe 反汇编里抄成基线。

问题：`TextFormat(118)` 的 6 个参数里，`arg1/arg2` 写进 `tw+0x5A8/+0x5AA`，但
`ref/exe-text-fields.json` 已证明这两个字段**没有任何读点**（`TextSpace(128)`
写的也是这两个字段）——也就是说它们在原版里是死存储。真正被读的是 `arg4`：
它写 `tw+0x5AC`，同时被 `0x430E71` 抄进 `tw+0x5E8`（笔位/累计行宽），而 `+0x5E8`
正是换行判据里被比较的量。本脚本把这条链的每个指令站点固定下来。

产物 `ref/exe-text-layout.json` 只含地址与指令文本，不含游戏数据。

用法：
    python scripts/extract-exe-text-layout.py            # 打印 + 写 json
    python scripts/extract-exe-text-layout.py --check    # 只比对（门禁用）
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
FIELDS = os.path.join(ROOT, 'ref', 'exe-text-fields.json')
OUT = os.path.join(ROOT, 'ref', 'exe-text-layout.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# 每个站点：(pc, 期望指令前缀, 角色)
SITES = [
    ('0x430e67', 'mov    0x5ac(%esi),%ax',
     'TextFormat arg4 (tw+0x5AC) 被读进 ax'),
    ('0x430e72', 'mov    %ax,0x5e8(%esi)',
     '文本复位：笔位/累计行宽 tw+0x5E8 := arg4'),
    ('0x4247c4', 'add    %ax,0x5e8(%ecx)',
     '逐字推进：tw+0x5E8 += 字宽'),
    ('0x4247da', 'add    %ax,0x5e8(%ecx)',
     '逐字推进（第二处）：tw+0x5E8 += 字宽'),
    ('0x432b02', 'mov    %cx,0x5e8(%esi)',
     '笔位 := 字符记录 +0x1C（排版器给的 x）'),
    ('0x433006', 'movswl 0x5e8(%esi),%ecx',
     '换行判据：读笔位'),
    ('0x433011', 'cmp    %edx,%ecx',
     '换行判据：笔位+字宽 与 (tw+0x5DC - 字距) 比较'),
    ('0x4330a8', 'mov    %cx,0x5e8(%esi)',
     '换行后：笔位恢复为行首保存值'),
]


def load_rows(path):
    rows = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE_RE.match(line)
            if not m:
                continue
            rows[int(m.group(1), 16)] = m.group(3).strip()
    return rows


def build():
    """从 asm + 字段表算出基线；asm 不在时返回 None。"""
    if not os.path.isfile(ASM):
        return None
    rows = load_rows(ASM)
    sites = []
    for pc_s, want, role in SITES:
        pc = int(pc_s, 16)
        got = rows.get(pc, '')
        sites.append(dict(pc=pc_s, instr=got, expect=want, role=role,
                          ok=got.startswith(want)))
    dead = {}
    if os.path.isfile(FIELDS):
        with open(FIELDS, encoding='utf-8') as f:
            fields = json.load(f)['fields']
        for off in ('0x5a8', '0x5aa', '0x5ac'):
            e = fields.get(off, {})
            dead[off] = dict(reads=[r['pc'] for r in e.get('reads', [])],
                             covering=[r['pc'] for r in e.get('covering_reads', [])],
                             name=e.get('name', ''))
    return dict(
        schema='fvp-text-layout/1',
        source='iroseka_HD.asm（objdump 反汇编）',
        claim=('TextFormat(118) 的 arg1/arg2（tw+0x5A8/+0x5AA，TextSpace(128) 也写这两个）'
               '在原版里没有读点，是死存储；arg4 经 tw+0x5AC 抄进 tw+0x5E8（笔位），'
               '参与换行判据并被逐字推进'),
        sites=sites, fields=dead)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    payload = build()
    if payload is None:
        print('SKIP: %s not found' % ASM)
        return 0
    sites = payload['sites']
    bad = [s for s in sites if not s['ok']]
    for s in sites:
        print('%s %-6s %-30s %s' % (s['pc'], 'ok' if s['ok'] else 'DIFF', s['instr'], s['role']))
    for off, e in payload['fields'].items():
        print('%s reads=%s covering=%s' % (off, e['reads'], e['covering']))
    if a.check:
        return 0 if not bad else 1
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())