#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 抄出 GraphLoad(22) 的实参规则（A 类：ref/exe-graph-load.json）。

handler `0x43A340`：
  arg0 id **0..0xFFF**（允许 0）
  arg1 按 Variant 类型分派：
      type 0（null/false）→ 0x424150：**释放该槽**
      type 4（数据段串偏移）→ *(engine+0x68DFAC) + payload
      type 5（动态串表槽号）→ *(engine+0x6A49EC + idx*4)
      其它 → 忽略
  解出非 NULL 串 → 0x428BD0(sprite_entry, str, 0)，随后 0x437330(engine+0x45C, id)

用法：
    python scripts/extract-exe-graph-load.py            # 打印 + 写 json
    python scripts/extract-exe-graph-load.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-graph-load.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

EXPECT = [
    (0x43A345, 'cmpb   $0x2,(%esi)'),
    (0x43A34D, 'test   %eax,%eax'),
    (0x43A351, 'cmp    $0xfff,%eax'),
    (0x43A358, 'cmpb   $0x0,0x8(%esi)'),
    (0x43A35E, 'imul   $0x51c,%eax,%eax'),
    (0x43A36B, 'call   0x424150'),
    (0x43A377, 'cmp    $0x4,%dl'),
    (0x43A37C, 'mov    0x68dfac(%ecx),%edx'),
    (0x43A387, 'cmp    $0x5,%dl'),
    (0x43A38F, 'mov    0x6a49ec(%ecx,%edx,4),%edx'),
    (0x43A3AA, 'call   0x428bd0'),
    (0x43A3C3, 'jmp    0x437330'),
]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺反汇编清单 %s' % ASM)
        return 0
    rows = {}
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if m:
            rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()
    checked = []
    for addr, want in EXPECT:
        got = rows.get(addr)
        if got != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        checked.append({'pc': '0x%06x' % addr, 'instr': want})

    result = {
        'note': 'GraphLoad(22) 的实参规则（A 类）；由 '
                'scripts/extract-exe-graph-load.py 生成',
        'id': 22,
        'name': 'GraphLoad',
        'handler': '0x0043a340',
        'argc': 2,
        'arg0': {'role': 'slot', 'range': [0, 0xFFF],
                 'note': '允许 0（sprite 表 4096 项）'},
        'arg1': {'role': 'name', 'dispatch': {
            'type0': 'null/false → 释放该槽（0x424150）',
            'type4': '数据段字符串偏移 → *(engine+0x68DFAC) + payload',
            'type5': '动态字符串表槽号 → *(engine+0x6A49EC + idx*4)',
            'other': '忽略'}},
        'then': '解出非 NULL 串 → 0x428BD0(sprite_entry, str, 0)，'
                '随后 0x437330(engine+0x45C, id)',
        'unknown': ['engine+0x45C 那个后处理到底做什么'],
        'instructions': checked,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== GraphLoad(22) ==')
    for k in ('arg0', 'arg1', 'then'):
        print('  %-5s %s' % (k, result[k]))
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
