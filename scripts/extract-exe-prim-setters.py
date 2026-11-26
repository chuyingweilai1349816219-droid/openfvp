#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里把**写图元结构体字段**的指令全抄出来（A 类：ref/exe-prim-setters.json）。

为什么：`PrimSetXY(90)/PrimSetWH(89)/PrimSetOP(81)/PrimSetBlend(78)` 的实参语义一直
靠"我们自己的实现"推断。本轮先在 exe 里做一次**全量扫描**：哪些指令真的往
0x34 步长的图元结构体里写 x/y/w/h/op1/op2/0x1C/0x1E？结果只有 40 条，集中在
0x43BA00~0x43BEC0 这一小段——也就是那几个 setter handler 的所在区间。

产物只记**事实**（pc + 指令 + 字段），不给"哪个 handler 是命令几"的猜测：
命令 id → handler 的映射要等命令分派表定位后才能钉死（见 unknown）。

用法：
    python scripts/extract-exe-prim-setters.py            # 打印 + 写 json
    python scripts/extract-exe-prim-setters.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-prim-setters.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
STORE = re.compile(r'^mov\s+%[a-z]{2},(0x[0-9a-f]+)\(%e[a-z]{2}\)$')
LO, HI = 0x43A000, 0x43C900
FIELDS = {
    '0x06': 'flags',
    '0x14': 'x',
    '0x16': 'y',
    '0x18': 'w',
    '0x1a': 'h',
    '0x1c': 'blend_a',
    '0x1e': 'blend_b',
    '0x20': 'op1',
    '0x22': 'op2',
    '0x24': 'angle',
    '0x26': 'scale_x',
    '0x28': 'scale_y',
    '0x2a': 'first_child',
    '0x32': 'text_layer',
}


def scan():
    out = []
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if not m:
            continue
        a = int(m.group(1), 16)
        if not (LO <= a <= HI):
            continue
        ins = m.group(3).split('#')[0].strip()
        sm = STORE.match(ins)
        if sm and sm.group(1) in FIELDS:
            out.append({'pc': '0x%06x' % a, 'field': FIELDS[sm.group(1)],
                        'offset': sm.group(1), 'instr': ins})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0
    stores = scan()
    clusters = []
    for s in stores:
        a = int(s['pc'], 16)
        if not clusters or a - clusters[-1][-1] > 0x120:
            clusters.append([a])
        else:
            clusters[-1].append(a)

    result = {
        'note': '写 0x34 步长图元结构体字段的指令全量扫描（A 类事实）；'
                '由 scripts/extract-exe-prim-setters.py 生成。'
                '命令 id 与 handler 的对应仍未钉死（要命令分派表）',
        'scan_range': ['0x43a000', '0x43c900'],
        'fields': {k: v for k, v in sorted(FIELDS.items())},
        'stores': stores,
        'clusters': [{'from': '0x%06x' % c[0], 'to': '0x%06x' % c[-1],
                      'count': len(c)} for c in clusters],
        'unknown': ['命令 78/81/89/90 各自对应哪个 handler（需要命令分派表地址）'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 写了图元字段的指令：%d 条 ==' % len(stores))
    for s in stores[:12]:
        print('  %s  %-9s %s' % (s['pc'], s['field'], s['instr']))
    print('  ...')
    print('== 聚簇（相邻 ≤0x120 字节）==')
    for c in result['clusters']:
        print('  %s..%s  %d 条' % (c['from'], c['to'], c['count']))
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