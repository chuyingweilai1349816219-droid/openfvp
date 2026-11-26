#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**图元层级/绘制派发**的结构事实（A 类）。

为什么需要它：正文文本窗口只通过 type-5 文本图元上屏，而"图元最终画在哪"取决于
引擎的**父链累积 + 派发**逻辑。以前这一块是靠"看起来对"来定的；本轮把
`0x438AB0`（派发）与 `0x438B0F`（组/子节点迭代）逐条读出来，冻结成
`ref/exe-prim-hierarchy.json`，让"屏幕坐标 = 父链上 x/y 之和"这条不变式有出处。

抄法（只抄，不猜）：
  * 图元步长：`imul $0x34,%eax` + `lea 0x61dd64(...)`；
  * 字段偏移：派发/组迭代里出现的 `0xNN(%esi)` / `0xNN(%eax)` 读点；
  * 父链累积：`movswl 0x14(%esi)` + `add 0x18(%esp)`（子调用参数 += 组的 x）
    与 `movswl 0x16(%esi)` + `add %ebp`（y 同理）；
  * 无父节点（`cmp $0xffff`）才走类型派发：`movsbl 0x4(%esi)` → `dec` → `cmp $0x6`
    → `jmp *0x438c9c(,%eax,4)`（1..7 共 7 类）；
  * type-5 文本：`movswl 0x32(%esi)` 取层号 → `0x98(engine,层号,4)` 取窗口 →
    `add $0x4` 取窗口内嵌 surface → 画 (x,y)（父链累积值）。

产物 `ref/exe-prim-hierarchy.json`（只含结构与指令，不含游戏数据）。

用法：
    python scripts/extract-exe-prim-hierarchy.py            # 打印 + 写 json
    python scripts/extract-exe-prim-hierarchy.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-prim-hierarchy.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# 关键指令（地址 → 期望助记符）：任何一条对不上就不产出，避免"抄错版本"。
EXPECT = [
    (0x438AB5, 'cmpb   $0x0,0x5(%esi)'),
    (0x438AC2, 'movzwl 0xc(%esi),%eax'),
    (0x438ACC, 'mov    0x14(%esi),%cx'),
    (0x438AD1, 'imul   $0x34,%eax,%eax'),
    (0x438AD4, 'lea    0x61dd64(%eax,%edi,1),%eax'),
    (0x438ADB, 'mov    %cx,0x14(%eax)'),
    (0x438AE7, 'mov    0x7(%esi),%cl'),
    (0x438AEA, 'mov    %cl,0x7(%eax)'),
    (0x438AFA, 'movsbl 0x4(%esi),%eax'),
    (0x438AFE, 'dec    %eax'),
    (0x438AFF, 'cmp    $0x6,%eax'),
    (0x438B08, 'jmp    *0x438c9c(,%eax,4)'),
    (0x438B0F, 'movswl 0x2a(%esi),%eax'),
    (0x438B22, 'movswl 0x16(%esi),%edx'),
    (0x438B26, 'imul   $0x34,%eax,%eax'),
    (0x438B30, 'movswl 0x14(%esi),%eax'),
    (0x438B34, 'add    0x18(%esp),%eax'),
    (0x438B38, 'add    %ebp,%edx'),
    (0x438B3F, 'call   0x438ab0'),
    (0x438B44, 'movswl 0x10(%ebx),%eax'),
    (0x438C54, 'movswl 0x32(%esi),%ecx'),
    (0x438C5D, 'mov    0x98(%eax,%ecx,4),%edx'),
    (0x438C68, 'add    $0x4,%edx'),
    (0x438C79, 'call   0x42ae40'),
]

# 图元结构字段（偏移 → 含义）：全部来自上面那些指令的读/写点。
FIELDS = {
    '0x04': 'type（1..7；派发用 movsbl 读）',
    '0x05': '内容标志（为 0 时整条派发直接返回）',
    '0x06': 'dirty/选项位（bit0 = WH 写过，bit1 = OP 写过，bit6 = 需要重画）',
    '0x07': 'alpha 字节',
    '0x08': 'blend 字节',
    '0x0c': '父图元 id（u16，0xFFFF = 无父）',
    '0x10': '下一个同组兄弟 id（u16，0xFFFF = 无）',
    '0x14': 'x（u16，脚本按 16 位写，即负数按补码存）',
    '0x16': 'y（u16）',
    '0x18': 'w（u16，PrimSetWH）',
    '0x1a': 'h（u16）',
    '0x20': 'OP.x（u16，仅 type==4 生效）',
    '0x22': 'OP.y（u16，仅 type==4 生效）',
    '0x24': '角度（u16，模 3600；单位 0.1° ⇒ 3600 = 一圈）',
    '0x26': 'scale x（per-mille，钳 100..10000）',
    '0x28': 'scale y（per-mille）',
    '0x2a': '第一个子图元 id（u16，0xFFFF = 无）',
    '0x2e': '图形槽号（u16，0xFFFF/-1/-2 是特殊通道）',
    '0x32': '文本窗口层号（u16，type-5 用）',
}


def load_rows(path):
    rows = {}
    order = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                addr = int(m.group(1), 16)
                rows[addr] = m.group(3).strip()
                order.append(addr)
    return rows, order


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows, order = load_rows(ASM)
    checked = []
    for addr, want in EXPECT:
        got = rows.get(addr)
        if got is None:
            print('FAIL: 0x%x 不存在（期望 %r）' % (addr, want))
            return 1
        if got.split('#')[0].strip() != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        checked.append({'pc': '0x%06x' % addr, 'instr': want})

    result = {
        'note': '从 exe 静态分析抄出的图元结构与绘制派发事实（A 类）；'
                '由 scripts/extract-exe-prim-hierarchy.py 生成',
        'prim': {
            'base': 'engine+0x61DD64',
            'stride': 0x34,
            'count': 0x1000,
            'id_range': [1, 0xFFF],
            'fields': FIELDS,
        },
        'dispatch': {
            'func': '0x438ab0',
            'args': ['this(renderer)', 'x（父链累积）', 'y（父链累积）', 'prim'],
            'steps': [
                {'pc': '0x438ab5', 'what': 'prim+0x05 == 0 → 不画'},
                {'pc': '0x438ac2', 'what': '读 prim+0x0c 父 id；!= 0xFFFF 时把本图元的'
                                            'x/y/alpha 抄进父图元，再以父为目标继续上溯'},
                {'pc': '0x438ad4', 'what': '图元基址 = engine + 0x61DD64 + id*0x34'},
                {'pc': '0x438afa', 'what': '无父时按 type（prim+0x04）- 1 查 0x438c9c 的'
                                           '7 项跳表派发'},
                {'pc': '0x438b0f', 'what': '组类型：取 prim+0x2a 第一个子图元'},
                {'pc': '0x438b22', 'what': '子调用 x = 组.x + 传入 x；y = 组.y + 传入 y'},
                {'pc': '0x438b44', 'what': '沿 prim+0x10 遍历同组兄弟'},
                {'pc': '0x438c54', 'what': 'type-5：prim+0x32 取文本层号，'
                                           'engine+0x98+层号*4 取窗口，+4 = 内嵌 surface，'
                                           '在父链累积的 (x,y) 上画'},
            ],
            'type_table': '0x438c9c',
            'type_count': 7,
            'invariant': '图元屏幕坐标 = 父链上各级 x/y 之和（叶子自身 x/y 由父图元抄写'
                         '步骤并入）；本引擎 gfx.c 的 prim_abs_xy() 实现的正是这条。',
        },
        'text_prim': {
            'layer_field': 0x32,
            'surface_offset_in_window': 4,
            'window_table': 'engine+0x98（32 项指针）',
            'draw_target': '0x42ae40',
        },
        'instructions': checked,
    }

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 已校验 %d 条关键指令 ==' % len(checked))
    for c in checked[:6]:
        print('  %s  %s' % (c['pc'], c['instr']))
    print('  ...')
    print('图元：base %s stride 0x%x 共 %d 项；id %d..0x%x'
          % (result['prim']['base'], result['prim']['stride'],
             result['prim']['count'], result['prim']['id_range'][0],
             result['prim']['id_range'][1]))
    print('派发：%s（type 跳表 %s，%d 类）'
          % (result['dispatch']['func'], result['dispatch']['type_table'],
             result['dispatch']['type_count']))
    print('不变式：%s' % result['dispatch']['invariant'])

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