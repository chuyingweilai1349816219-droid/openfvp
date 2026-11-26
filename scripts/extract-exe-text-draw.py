#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**文本面板的落点与尺寸**（A 类：type-5 到底画在哪、多大）。

为什么需要它：正文文本的位置一直卡在"面板是不是画在 prim 的 x/y 上"。以前只能靠
"看起来对"；本轮把 `0x42AE40`（type-4/5 共用的贴图绘制）的目标矩形算法读通：

  * 调用约定：`0x42AE40(this(renderer), argx, argy, prim, surf)`，其中 (argx,argy)
    是**父链累积**的坐标（type-5 在 `0x438C54` 里把累积值透传进来）；
  * 尺寸：prim 的 WH 写过（`prim+0x06` bit0）就用 `prim+0x18/+0x1a`，
    否则用 **surface 自己的 `+0x510/+0x512`**（= 面板尺寸）；
  * 落点：prim 的 OP 写过（bit1）时是
    `x = prim.x + prim.OPx + argx`、`y = prim.y + prim.Opx + argy`（`0x42B429..`）；
    否则是 `x = prim.x + surf[0x518] + argx`、`y = prim.y + surf[0x51a] + argy`
    （`0x42B484..`）；
  * `surf+0x514/+0x516` 是**源裁剪原点**（缩放分支里被减掉），不是落点；
  * PrimSetText(86) 会**清掉 WH 位**（`0x43BDE1 andb $0xfe,0x6(%eax)`），
    所以文本面板永远走"尺寸 = 面板尺寸"这一支。

结论（进 JSON 的 `conclusion`）：**文本面板 1:1 画在 prim 的 (x,y) 上，尺寸就是
TextBuff 建的 panel 尺寸**，不缩放、不加版式偏移。所以正文窗口的位置就是脚本设的
(431,880)，面板 1380×1024。

产物 `ref/exe-text-draw.json`（只含结构与指令，不含游戏数据）。

用法：
    python scripts/extract-exe-text-draw.py            # 打印 + 写 json
    python scripts/extract-exe-text-draw.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-draw.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# 关键指令（地址 → 期望助记符）：任何一条对不上就不产出。
EXPECT = [
    (0x438C54, 'movswl 0x32(%esi),%ecx'),        # type-5：取文本层号
    (0x438C5D, 'mov    0x98(%eax,%ecx,4),%edx'),  # 窗口表
    (0x438C68, 'add    $0x4,%edx'),               # 内嵌 surface
    (0x438C79, 'call   0x42ae40'),                # 透传父链累积的 (x,y)
    (0x42AE50, 'cmpb   $0x0,0x508(%ebx)'),        # surface 有效位
    (0x42AE64, 'cmpb   $0x0,0x7(%edi)'),          # prim alpha
    (0x42AEC8, 'testb  $0x1,0x6(%edi)'),          # WH 写过？
    (0x42AEF6, 'movswl 0x510(%ebx),%eax'),        # 没写过 → 用 surface 的宽
    (0x42AEFD, 'movswl 0x512(%ebx),%edx'),        # 没写过 → 用 surface 的高
    (0x42AF08, 'testb  $0x2,0x6(%edi)'),          # OP 写过？
    (0x42AF10, 'movswl 0x514(%ebx),%eax'),        # 源裁剪原点 x
    (0x42AF30, 'movswl 0x518(%ebx),%edx'),        # 落点偏移 x（无 OP 时）
    (0x42B41F, 'testb  $0x2,0x6(%edi)'),          # 落点分支：OP
    (0x42B429, 'movswl 0x22(%edi),%edx'),         # OP.y
    (0x42B42D, 'add    0xc(%ebp),%edx'),          # + argy
    (0x42B430, 'movswl 0x16(%edi),%eax'),         # prim.y
    (0x42B438, 'add    0x8(%ebp),%ecx'),          # + argx
    (0x42B43B, 'add    %edx,%eax'),               # y = prim.y + OPy + argy
    (0x42B445, 'add    %ecx,%edx'),               # x = prim.x + OPx + argx
    (0x42B484, 'movswl 0x51a(%ebx),%ecx'),        # 无 OP：surf 落点偏移 y
    (0x42B48B, 'add    0xc(%ebp),%ecx'),          # + argy
    (0x42B492, 'movswl 0x518(%ebx),%eax'),        # surf 落点偏移 x
    (0x42B499, 'add    0x8(%ebp),%eax'),          # + argx
    (0x43BDE1, 'andb   $0xfe,0x6(%eax)'),         # PrimSetText 清 WH 位
]

FIELDS = {
    'prim+0x06.bit0': 'WH 写过（PrimSetText 会清掉，所以文本面板用面板尺寸）',
    'prim+0x06.bit1': 'OP 写过（决定落点用 OP 还是 surface 偏移）',
    'prim+0x14': 'x（落点基准）',
    'prim+0x16': 'y（落点基准）',
    'prim+0x18': 'w（WH 写过时用）',
    'prim+0x1a': 'h（WH 写过时用）',
    'prim+0x20': 'OP.x（OP 写过时加到落点）',
    'prim+0x22': 'OP.y',
    'surface+0x510': 'surface 宽（WH 没写过时当绘制宽）',
    'surface+0x512': 'surface 高（WH 没写过时当绘制高）',
    'surface+0x514': '源裁剪原点 x（缩放分支里减掉）',
    'surface+0x516': '源裁剪原点 y',
    'surface+0x518': '落点偏移 x（无 OP 分支里加上）',
    'surface+0x51a': '落点偏移 y',
}


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
        'note': '从 exe 静态分析抄出的文本面板落点/尺寸规则（A 类）；'
                '由 scripts/extract-exe-text-draw.py 生成',
        'call': {
            'dispatcher': '0x438ab0',
            'draw': '0x42ae40',
            'args': ['this(renderer)', 'argx（父链累积）', 'argy（父链累积）',
                     'prim', 'surf（type-5 时 = 窗口内嵌 surface）'],
            'type5_site': '0x438c54',
        },
        'fields': FIELDS,
        'size': {
            'if_wh_written': 'prim+0x18 / prim+0x1a',
            'else': 'surf+0x510 / surf+0x512（= 面板尺寸）',
            'why_text_uses_panel': 'PrimSetText(86) 的 0x43BDE1 清掉 prim+0x06 bit0，'
                                   '所以文本面板永远走 else 分支',
        },
        'position': {
            'if_op_written': 'x = prim+0x14 + prim+0x20 + argx；'
                             'y = prim+0x16 + prim+0x22 + argy',
            'else': 'x = prim+0x14 + surf+0x518 + argx；'
                    'y = prim+0x16 + surf+0x51a + argy',
            'source_crop': 'surf+0x514 / surf+0x516 是**源裁剪原点**（缩放分支里被减掉，不是落点）',
        },
        'conclusion': '文本面板 1:1 画在 prim 的 (x,y) 上（surface 偏移默认 0），'
                      '尺寸 = TextBuff 建出来的面板尺寸；不缩放、没有版式偏移。'
                      '本引擎 gfx.c 的 type-5 路径（prim_abs_xy → draw_text_window）'
                      '实现的正是这条。',
        'instructions': checked,
    }

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 已校验 %d 条关键指令 ==' % len(checked))
    for c in checked[:6]:
        print('  %s  %s' % (c['pc'], c['instr']))
    print('  ...')
    print('尺寸：WH 写过 → prim w/h；否则 → surface 0x510/0x512（面板尺寸，文本走这条）')
    print('落点：OP 写过 → prim + OP + 累积；否则 → prim + surface 0x518/0x51a + 累积')
    print('结论：%s' % result['conclusion'])

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