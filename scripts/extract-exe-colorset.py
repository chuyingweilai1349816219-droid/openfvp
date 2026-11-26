#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 抄出 ColorSet(7) 的实参规则（A 类：ref/exe-colorset.json）。

handler `0x4373E0`：
  arg0 index int **0..0xFF**
  arg1 R -> engine+0x6A44C2 + idx*4     arg2 G -> +0x6A44C1 + idx*4
  arg3 B -> engine+0x6A44C0 + idx*4     arg4 A -> +0x6A44C3 + idx*4
  每个分量各自判 type==2，非 int 跳过；无范围检查（截断成字节）。

所以 engine+0x6A44C0 是 **256 项 BGRA 调色板**（读档第 19 段就是它）。

用法：
    python scripts/extract-exe-colorset.py            # 打印 + 写 json
    python scripts/extract-exe-colorset.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-colorset.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

EXPECT = [
    (0x4373E9, 'cmp    %cl,(%eax)'),
    (0x4373F0, 'test   %edx,%edx'),
    (0x4373F4, 'cmp    $0x100,%edx'),
    (0x437400, 'jne    0x43740c'),
    (0x437405, 'mov    %bl,0x6a44c2(%esi,%edx,4)'),
    (0x437414, 'mov    %bl,0x6a44c1(%esi,%edx,4)'),
    (0x437423, 'mov    %bl,0x6a44c0(%esi,%edx,4)'),
    (0x437433, 'mov    %al,0x6a44c3(%esi,%edx,4)'),
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
        'note': 'ColorSet(7) 的实参规则（A 类）；由 '
                'scripts/extract-exe-colorset.py 生成',
        'id': 7, 'name': 'ColorSet', 'handler': '0x004373e0', 'argc': 5,
        'index_range': [0, 0xFF],
        'palette': {'base': 'engine+0x6A44C0', 'entries': 256, 'order': 'BGRA',
                    'component_offsets': {'B': 0, 'G': 1, 'R': 2, 'A': 3}},
        'dispatch': {'R': 'arg1 -> +2', 'G': 'arg2 -> +1',
                     'B': 'arg3 -> +0', 'A': 'arg4 -> +3'},
        'rules': ['每个分量各自判 type==2，非 int 的分量保持原值',
                  '分量没有范围检查（截断成字节）',
                  'index 必须 int 且 0..0xFF，否则整条忽略'],
        'readers': ['图元绘制 type-2（tile）路径：mov 0x6a44c0(%edi,%eax,4),%edi'],
        'instructions': checked,
        'unknown': ['tile 绘制时索引色查表的完整算法'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== ColorSet(7) ==')
    print('  调色板 %s，%d 项 %s' % (result['palette']['base'],
                                     result['palette']['entries'],
                                     result['palette']['order']))
    for r in result['rules']:
        print('  -', r)
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
