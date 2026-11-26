#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图元字段的**位宽**：写侧 u16、读侧 movswl ⇒ 语义是 int16（A 类基线）。

写侧证据在 ref/exe-prim-setters-args.json（`mov %xx,off(%eax)`，都是 16 位寄存器写）。
本脚本补**读侧**证据：在图元绘制/分派区（0x424xxx~0x42Bxxx）里，每个字段都被
`movswl off(%reg),%reg` 读出——按有符号 16 位扩展，所以范围是 -32768..32767。

产物 `ref/exe-prim-fields-width.json`。

用法：
    python scripts/extract-exe-prim-fields-width.py            # 打印 + 写 json
    python scripts/extract-exe-prim-fields-width.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-prim-fields-width.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# 每个字段挑一条"绘制/分派区里用 movswl 读"的指令作为证据
EXPECT = [
    ('x', 0x42450E, 'movswl 0x14(%esi),%eax'),
    ('y', 0x424518, 'movswl 0x16(%esi),%edi'),
    ('w', 0x42541C, 'movswl 0x18(%edi),%ecx'),
    ('h', 0x424665, 'movswl 0x1a(%edx),%edx'),
    ('u', 0x42AED5, 'movswl 0x1c(%edi),%ecx'),
    ('v', 0x42AEE2, 'movswl 0x1e(%edi),%eax'),
    ('op1', 0x42450A, 'movswl 0x20(%esi),%edx'),
    ('op2', 0x42AF2A, 'movswl 0x22(%edi),%ebx'),
    ('angle', 0x424596, 'movswl 0x24(%edx),%esi'),
    ('scale', 0x42B25A, 'movswl 0x26(%edi),%edx'),
]

# 写侧：字段 -> 写它的 handler（来自 ref/exe-prim-setters-args.json 的手工对应）
WRITE_SIDE = {
    'x': ['0x43bfa0', '0x43bdc0', '0x43be40', '0x43bc40', '0x43bd00'],
    'y': ['0x43bfa0', '0x43bdc0', '0x43be40', '0x43bc40', '0x43bd00'],
    'w': ['0x43bf50', '0x43be40'],
    'h': ['0x43bf50', '0x43be40'],
    'u': ['0x43bef0'],
    'v': ['0x43bef0'],
    'op1': ['0x43ba90'],
    'op2': ['0x43ba90'],
    'angle': ['0x43baf0'],
    'scale': ['0x43baf0'],
}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0
    rows = {}
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if m:
            rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()

    fields = []
    for name, addr, want in EXPECT:
        got = rows.get(addr)
        if got != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        size = re.search(r'0x([0-9a-f]+)\(', want).group(1)
        fields.append({
            'field': name,
            'offset': '0x' + size,
            'write_width': 'u16（写侧 handler 用 mov %xx,off）',
            'read_width': 'movswl（有符号 16 位扩展）',
            'read_pc': '0x%06x' % addr,
            'read_instr': want,
            'written_by': WRITE_SIDE[name],
        })

    result = {
        'note': '图元字段的位宽证据（A 类）：写侧 u16、读侧 movswl ⇒ 语义 int16'
                '（-32768..32767）；由 scripts/extract-exe-prim-fields-width.py 生成',
        'why': '本轮 src/syscall.c 的 case 81/89/90 按 int16 截断（FVP_S16）的依据',
        'fields': fields,
        'range': [-32768, 32767],
        'unknown': ['prim+0x06 的 0x40 位在渲染端如何使用'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 字段位宽 ==')
    for f in fields:
        print('  %-6s %-6s %-30s %s' % (f['field'], f['offset'],
                                        f['read_instr'], f['read_pc']))
    print('⇒ 范围 %d..%d' % (result['range'][0], result['range'][1]))
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