#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图元**类型分派表**（A 类：ref/exe-prim-type-dispatch.json）。

`0x438AFA` 起是类型分派：

    438afa: movsbl 0x4(%esi),%eax     ; type = (int8)prim+0x04
    438afe: dec    %eax               ; index = type - 1
    438aff: cmp    $0x6,%eax
    438b02: ja     0x438c94           ; 越界 → 直接返回
    438b08: jmp    *0x438c9c(,%eax,4) ; 7 项跳表，覆盖 type 1..7

跳表内容（0x438C9C）：
    type 1 → 0x438b0f   type 2 → 0x438be1   type 3 → 0x438c94（空）
    type 4 → 0x438b54   type 5 → 0x438c54   type 6 → 0x438c94（空）
    type 7 → 0x438c83

其中 type 5 的落点 `0x438C54` 会调 `0x42AE40`（文字面板绘制，我们前面已冻结）；
type 7 的落点 `0x438C83` 调 `0x42AA10`（**snow 绘制**），后者用
`prim+0x2E` 当"雪系统"下标：`engine+0x684378 + idx*0x5048`，启用标志在
`engine+0x689378 + idx*0x5048`。

用法：
    python scripts/extract-exe-prim-type-dispatch.py            # 打印 + 写 json
    python scripts/extract-exe-prim-type-dispatch.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-prim-type-dispatch.json')

IMAGE_BASE = 0x400000
LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

EXPECT = [
    (0x438AFA, 'movsbl 0x4(%esi),%eax'),
    (0x438AFE, 'dec    %eax'),
    (0x438AFF, 'cmp    $0x6,%eax'),
    (0x438B08, 'jmp    *0x438c9c(,%eax,4)'),
    (0x438C83, 'mov    0x481368,%eax'),
    (0x438C8F, 'call   0x42aa10'),
    (0x438C79, 'call   0x42ae40'),
    (0x42AA23, 'movswl 0x2e(%eax),%ecx'),
    (0x42AA2C, 'imul   $0x5048,%ecx,%ecx'),
    (0x42AA32, 'cmpb   $0x0,0x689378(%ecx,%eax,1)'),
    (0x42AA3B, 'lea    0x684378(%ecx,%eax,1),%edi'),
]


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM) or not os.path.isfile(EXE):
        print('SKIP: 缺反汇编清单或 exe')
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

    buf = io.open(EXE, 'rb').read()
    pe = struct.unpack_from('<I', buf, 0x3C)[0]
    nsec = struct.unpack_from('<H', buf, pe + 6)[0]
    optsize = struct.unpack_from('<H', buf, pe + 20)[0]
    secs = []
    for i in range(nsec):
        s = pe + 24 + optsize + i * 40
        nm = buf[s:s + 8].rstrip(b'\x00').decode('latin1')
        vsize, vaddr, rawsize, rawptr = struct.unpack_from('<IIII', buf, s + 8)
        secs.append((nm, vaddr, vsize, rawptr, rawsize))

    def va2off(va):
        rva = va - IMAGE_BASE
        for nm, vaddr, vsize, rawptr, rawsize in secs:
            if vaddr <= rva < vaddr + vsize:
                return rawptr + (rva - vaddr)
        return None

    base = va2off(0x438C9C)
    targets = []
    for i in range(7):
        v = struct.unpack_from('<I', buf, base + i * 4)[0]
        targets.append('0x%06x' % v)

    NAMES = {1: 'group', 2: 'tile', 3: '（空）', 4: 'sprite', 5: 'text',
             6: '（空）', 7: 'snow'}
    result = {
        'note': '图元类型分派表（A 类）：type = (int8)prim+0x04，index = type-1，'
                '跳表在 0x438C9C，覆盖 type 1..7；由 '
                'scripts/extract-exe-prim-type-dispatch.py 生成',
        'dispatch_pc': '0x00438afa',
        'table_va': '0x00438c9c',
        'types': [{'type': t, 'name': NAMES[t], 'target': targets[t - 1]}
                  for t in range(1, 8)],
        'targets': {
            'type5_calls': '0x42ae40（文字面板绘制，与 ref/exe-text-draw.json 一致）',
            'type7_calls': '0x42aa10（snow 绘制）',
        },
        'snow_system': {
            'fn': '0x42aa10',
            'selector': 'prim+0x2E（有符号 u16）当雪系统下标',
            'record': 'engine+0x684378 + idx*0x5048（共 0x5048 字节/条）',
            'enabled_flag': 'engine+0x689378 + idx*0x5048（0 = 不画）',
            'note': '雪系统容器正是读档恢复顺序里 0x43F3D4 恢复的那个 '
                    '0x431E70(engine+0x684378)',
        },
        'instructions': checked,
        'unknown': ['雪系统记录 0x5048 字节里各字段的含义（浮点参数/frame 等）',
                    'type 2/3/4/6 的落点还没逐个读'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 类型分派 ==')
    for t in result['types']:
        print('  type %d  %-6s → %s' % (t['type'], t['name'], t['target']))
    print('== snow（type 7）==')
    for k, v in result['snow_system'].items():
        print('  %-14s %s' % (k, v))
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