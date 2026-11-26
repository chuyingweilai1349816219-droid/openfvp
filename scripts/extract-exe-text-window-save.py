#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**文本窗口的存档格式**（A 类：`ref/exe-text-window-save.json`）。

为什么需要它：读档恢复顺序第 29 步是"32 个文本窗口 × `0x433850`"，也就是**文本窗口
（面板尺寸/颜色/格式/笔位/文本本身）确实存在存档里**。要把 `--load-restart` 做成真
恢复，就必须知道这段的字段顺序。本轮把 `0x433850` 的读取序列逐条抄出来：

  1. `u8`  存在标志（0 = 该窗口为空，直接跳过）
  2. surface 记录：`0x429190(win+4, engine)`（与 sprite 表同一套 surface 反序列化）
  3. 字符串：`0x4303A0(win+0x574, engine)`（窗口文本）
  4. `u32` 计数，随后 count × `u16`（字格）
  5. `u32` → `win+0x58C`
  6. memcpy **0x28 字节** → `win+0x590`（文字样式块）
  7. 计数向量 → `win+0x5B8`（`0x430430`）
  8. 计数向量 → `win+0x5EC`（`0x42F620`）
  9. `u32` → `win+0x5D0`
 10. `u8` × 4 → `win+0x5D5/0x5D6/0x5D7/0x5DA`
 11. `u16` × 2 → `win+0x5DC/0x5DE`（**面板宽高**）
 12. `u32` × 2 → `win+0x5E0/0x5E4`
 13. `u16` × 2 → `win+0x5E8/0x5EA`（**排版笔位**）

产物 `ref/exe-text-window-save.json`。

用法：
    python scripts/extract-exe-text-window-save.py            # 打印 + 写 json
    python scripts/extract-exe-text-window-save.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-window-save.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# 关键指令（地址 → 期望助记符）：任何一条对不上就不产出
EXPECT = [
    (0x433860, 'movsbl (%eax,%ecx,1),%ecx'),      # 存在标志
    (0x433876, 'movb   $0x1,0x5d4(%edi)'),
    (0x43387D, 'call   0x429190'),                # surface 记录
    (0x4338C9, 'call   0x4303a0'),                # 字符串
    (0x4338D5, 'mov    (%ecx),%ecx'),             # u32 计数
    (0x4338EA, 'mov    (%edx,%eax,1),%cx'),       # u16 × count
    (0x43395C, 'mov    %ecx,0x58c(%edi)'),
    (0x43396C, 'push   $0x28'),                   # 样式块 0x28 字节
    (0x433976, 'call   0x447800'),
    (0x4339CE, 'call   0x430430'),                # 向量 → 0x5B8
    (0x4339DA, 'mov    (%ecx),%ecx'),             # 向量计数
    (0x433A32, 'mov    %eax,0x5d0(%edi)'),
    (0x433A43, 'mov    %dl,0x5d5(%edi)'),
    (0x433A5D, 'mov    %al,0x5d6(%edi)'),
    (0x433A70, 'mov    %al,0x5d7(%edi)'),
    (0x433A83, 'mov    %al,0x5da(%edi)'),
    (0x433A95, 'mov    %dx,0x5dc(%edi)'),         # 面板宽
    (0x433AB0, 'mov    %ax,0x5de(%edi)'),         # 面板高
    (0x433AC3, 'mov    %eax,0x5e0(%edi)'),
    (0x433AD5, 'mov    %eax,0x5e4(%edi)'),
    (0x433AE8, 'mov    %ax,0x5e8(%edi)'),         # 笔位 x
    (0x433AFC, 'mov    %ax,0x5ea(%edi)'),         # 笔位 y
]

FIELDS = [
    {'order': 1, 'size': 'u8', 'field': 'present',
     'pc': '0x433860', 'what': '存在标志（0 = 该窗口为空，直接跳过）'},
    {'order': 2, 'size': 'record', 'field': 'surface',
     'pc': '0x43387d', 'what': 'surface 记录，0x429190(win+4, engine)（与 sprite 同一套）'},
    {'order': 3, 'size': 'string', 'field': 'text',
     'pc': '0x4338c9', 'what': '窗口文本，0x4303A0(win+0x574, engine)'},
    {'order': 4, 'size': 'u32', 'field': 'cell_count',
     'pc': '0x4338d5', 'what': '随后字格的个数'},
    {'order': 5, 'size': 'u16×count', 'field': 'cells',
     'pc': '0x4338ea', 'what': '字格（u16 序列）'},
    {'order': 6, 'size': 'u32', 'field': 'win+0x58C',
     'pc': '0x43395c', 'what': 'win+0x58C'},
    {'order': 7, 'size': '0x28', 'field': 'style_block',
     'pc': '0x433976', 'what': 'memcpy 0x28 字节 → win+0x590（文字样式块）'},
    {'order': 8, 'size': 'vector', 'field': 'win+0x5B8',
     'pc': '0x4339ce', 'what': '计数向量 → win+0x5B8（0x430430）'},
    {'order': 9, 'size': 'vector', 'field': 'win+0x5EC',
     'pc': '0x4339da', 'what': '计数向量 → win+0x5EC（0x42F620）'},
    {'order': 10, 'size': 'u32', 'field': 'win+0x5D0',
     'pc': '0x433a32', 'what': 'win+0x5D0'},
    {'order': 11, 'size': 'u8', 'field': 'win+0x5D5',
     'pc': '0x433a43', 'what': 'win+0x5D5'},
    {'order': 12, 'size': 'u8', 'field': 'win+0x5D6',
     'pc': '0x433a5d', 'what': 'win+0x5D6'},
    {'order': 13, 'size': 'u8', 'field': 'win+0x5D7',
     'pc': '0x433a70', 'what': 'win+0x5D7（TextSkip 的 mode 也写这里）'},
    {'order': 14, 'size': 'u8', 'field': 'win+0x5DA',
     'pc': '0x433a83', 'what': 'win+0x5DA'},
    {'order': 15, 'size': 'u16', 'field': 'panel_w',
     'pc': '0x433a95', 'what': '面板宽 win+0x5DC'},
    {'order': 16, 'size': 'u16', 'field': 'panel_h',
     'pc': '0x433ab0', 'what': '面板高 win+0x5DE'},
    {'order': 17, 'size': 'u32', 'field': 'win+0x5E0',
     'pc': '0x433ac3', 'what': 'win+0x5E0'},
    {'order': 18, 'size': 'u32', 'field': 'win+0x5E4',
     'pc': '0x433ad5', 'what': 'win+0x5E4'},
    {'order': 19, 'size': 'u16', 'field': 'pen_x',
     'pc': '0x433ae8', 'what': '排版笔位 x（win+0x5E8）'},
    {'order': 20, 'size': 'u16', 'field': 'pen_y',
     'pc': '0x433afc', 'what': '排版笔位 y（win+0x5EA）'},
]


def load_rows(path):
    rows = {}
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()
    return rows


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0
    rows = load_rows(ASM)
    checked = []
    for addr, want in EXPECT:
        got = rows.get(addr)
        if got is None:
            print('FAIL: 0x%x 不存在（期望 %r）' % (addr, want))
            return 1
        if got != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        checked.append({'pc': '0x%06x' % addr, 'instr': want})

    result = {
        'note': '从 exe 静态分析抄出的文本窗口存档格式（A 类）；读档恢复顺序第 29 步用 '
                '0x433850 逐个恢复 32 个窗口；由 scripts/extract-exe-text-window-save.py 生成',
        'entry': '0x433850',
        'args': ['win（窗口对象）', 'engine（流对象：base 在 +0，游标在 +8）'],
        'stream': 'base = *(engine+0x0)，cursor = engine+0x8；每次读都把 cursor 往前推',
        'fields': FIELDS,
        'restored_by_load': {
            'loop_pc': '0x0043f38f',
            'count': 32,
            'window_table': 'engine+0x98，步长 4，下标 0x98/4..0x118/4',
            'what': '读档时 32 个窗口逐个调 0x433850 反序列化',
        },
        'instructions': checked,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 已校验 %d 条关键指令、%d 个字段 ==' % (len(checked), len(FIELDS)))
    for f in FIELDS[:6]:
        print('  #%-2d %-12s %-14s %s' % (f['order'], f['size'], f['field'],
                                          f['what']))
    print('  ...')
    print('读档：%d 个窗口逐个 0x433850（%s）'
          % (result['restored_by_load']['count'],
             result['restored_by_load']['window_table']))

    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())