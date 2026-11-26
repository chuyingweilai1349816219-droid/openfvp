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


# 这些偏移在 TextFormat(118)/TextFunction(119)/TextSpace(128) 里被写，
# 但全 exe 里没有任何**读**（源操作数）或读改写 —— 即"死存储"。
# arg4（0x5AC）不在这个表里，它是唯一被消费的参数。
DEAD_OFFSETS = [0x5A4, 0x5A5, 0x5A6, 0x5A8, 0x5AA, 0x5AE, 0x5B0, 0x5B4]

# TextSkip(127) 不是死存储：tw+0x5D7 的低两位各选一组输入掩码，命中就把
# tw+0x5D8 置 1；0x5D8 随后被三处比较、三处清零。站点如下（判据同 sites）。
TEXTSKIP_SITES = [
    ('0x43ce06', 'mov    %al,0x5d7(%ecx)',
     'TextSkip handler 写 tw+0x5D7'),
    ('0x431084', 'mov    0x5d7(%esi),%cl',
     '读 TextSkip 的两位'),
    ('0x43108c', 'test   $0x1,%cl',
     'bit0 -> 掩码 0x90'),
    ('0x431096', 'test   $0x2,%cl',
     'bit1 -> 掩码 0x60'),
    ('0x4310a4', 'test   %edx,0x43c(%ecx)',
     '与引擎输入掩码 engine+0x43C 求交'),
    ('0x4310ac', 'movb   $0x1,0x5d8(%esi)',
     '命中则把 tw+0x5D8 置 1'),
    ('0x43110d', 'cmp    %bl,0x5d8(%esi)',
     '0x5D8 的读点（比较）'),
    ('0x4335be', 'cmpb   $0x0,0x5d8(%esi)',
     '0x5D8 的读点（排版循环里判断）'),
    ('0x42db78', 'mov    0x5d7(%edi),%al',
     '存档序列化：把 0x5D7 的字节抄进流'),
]

READ_RE = re.compile(r'0x([0-9a-f]+)\(%e[a-z]{2}\),%[a-z0-9]+$')
WRITE_RE = re.compile(r'%[a-z0-9]+,0x([0-9a-f]+)\(%e[a-z]{2}\)$')
RMW_RE = re.compile(r'^(add|sub|and|or|xor|inc|dec)')
MEM_RE = re.compile(r'0x([0-9a-f]+)\(%e[a-z]{2}\)')


def scan_offset(rows, off):
    """直接扫反汇编：返回 (reads, writes, rmw) 的 pc 列表。"""
    reads, writes, rmw = [], [], []
    for pc in sorted(rows):
        instr = rows[pc]
        m = MEM_RE.search(instr)
        if not m or int(m.group(1), 16) != off:
            continue
        pc_s = '0x%x' % pc
        if READ_RE.search(instr):
            reads.append(pc_s)
        elif WRITE_RE.search(instr):
            writes.append(pc_s)
        if RMW_RE.match(instr):
            rmw.append(pc_s)
    return reads, writes, rmw


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
    skip = []
    for pc_s, want, role in TEXTSKIP_SITES:
        pc = int(pc_s, 16)
        got = rows.get(pc, '')
        skip.append(dict(pc=pc_s, instr=got, expect=want, role=role,
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
        claim=('TextFormat(118) 的 arg4 是唯一被消费的参数（行首缩进）；'
               '其余 5 个参数与 TextFunction(119) 的 3 个开关、TextSpace(128) 的 2 个分量'
               '都是死存储：全 exe 没有读点也没有读改写'),
        sites=sites, fields=dead, textskip=skip,
        textskip_note=('TextSkip(127) 的两位置 1 后各选一组输入掩码（bit0 -> 0x90、'
                       'bit1 -> 0x60），与 engine+0x43C 求交；命中则把 tw+0x5D8 置 1，'
                       '而 0x5D8 在 0x43110D / 0x43111C / 0x4335BE 被读取、在三处被清零。'
                       '所以它不是死存储；但"命中之后排版循环具体怎么跳过"尚未逐条跟完'),
        dead_fields=[dict(offset='0x%x' % off, reads=r, writes=w, rmw=m)
                     for off, (r, w, m) in
                     ((off, scan_offset(rows, off)) for off in DEAD_OFFSETS)])


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
    for d in payload.get('dead_fields', []):
        print('%s reads=%d writes=%d rmw=%d'
              % (d['offset'], len(d['reads']), len(d['writes']), len(d['rmw'])))
    if a.check:
        return 0 if not bad else 1
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())