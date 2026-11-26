#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出 **TextBuff(110) 的尺寸上限与越界行为**（A 类）。

为什么需要它：上一轮我怀疑"我们的引擎把 1380 宽的面板丢掉了、原版不会"，本轮把
`0x430EA0` 读透，结论是我们**没错**：

  * `0x430EA8 cmpl $0x400,w` / `0x430EBA cmpl $0x400,h`，任一 `jg` → `0x430FF8`；
  * `0x430FF8` 不是静默返回，而是 **抛异常**：`push $0x80` → `call 0x447C05`
    （构造异常对象）→ `push 0x400,0x400,0x45D8C4(格式串)` → `call 0x4258F0`
    （格式化错误信息）→ `call 0x44837C`（throw）；
  * 没越界时：`tw+0x5DC/0x5DE = w/h`（面板尺寸）、`call 0x4289F0` 建/改 surface、
    `w*h*4` 字节缓冲挂到 `tw+0x520`。

⇒ 规则是 **`w > 1024 或 h > 1024` 抛异常（code 0x80）**，不是"悄悄放弃"。
脚本里 `TextBuff(0, 1380, 1024)`（正文层，设计 736×202 换算后）会踩到这条——
所以原版里那段窗口设置会中断（除非脚本自己接住异常）。

产物 `ref/exe-textbuff.json`。

用法：
    python scripts/extract-exe-textbuff.py            # 打印 + 写 json
    python scripts/extract-exe-textbuff.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-textbuff.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

EXPECT = [
    (0x430EA8, 'cmp    $0x400,%edi'),        # w > 0x400 ?
    (0x430EB0, 'jg     0x430ff8'),           # 越界 → 异常分支
    (0x430EBA, 'cmp    $0x400,%ebp'),        # h > 0x400 ?
    (0x430EC0, 'jg     0x430ff8'),
    (0x430ECD, 'mov    %di,0x5dc(%esi)'),    # panel_w
    (0x430ED4, 'mov    %bp,0x5de(%esi)'),    # panel_h
    (0x430EDB, 'call   0x4289f0'),           # 建/改 surface
    (0x430EFB, 'imul   %ebp,%edi'),          # w*h
    (0x430F07, 'mul    %edx'),               # *4
    (0x430F11, 'call   0x4470eb'),           # 分配
    (0x430F22, 'mov    %eax,0x520(%esi)'),   # 缓冲指针
    (0x430FF8, 'push   $0x80'),              # 异常分支：错误码 0x80
    (0x430FFD, 'call   0x447c05'),           # 构造异常
    (0x43101B, 'call   0x4258f0'),           # 格式化错误信息
    (0x431035, 'call   0x44837c'),           # throw
    # handler 0x43C830：层号 0..0x1F、w/h 缺省 8、只有 int 且 >=0 才覆盖
    (0x43C834, 'cmpb   $0x2,(%eax)'),         # arg0 必须是 int
    (0x43C840, 'cmp    $0x1f,%ecx'),          # 层号 <= 0x1F
    (0x43C858, 'mov    $0x8,%edi'),           # w 缺省 8
    (0x43C85D, 'mov    %edi,%esi'),           # h 缺省 8
    (0x43C87B, 'call   0x430ea0'),            # 交给构造/改尺寸函数
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
        'note': '从 exe 静态分析抄出的 TextBuff(110) 尺寸上限与越界行为（A 类）；'
                '由 scripts/extract-exe-textbuff.py 生成',
        'handler': '0x430ea0',
        'limit': {
            'w_max': 0x400,
            'h_max': 0x400,
            'check_pc': ['0x430ea8', '0x430eba'],
            'on_overflow': '抛异常：错误码 0x80（0x430ff8 push $0x80），'
                           '0x447C05 构造 → 0x4258F0 格式化（格式串 0x45D8C4）→ '
                           '0x44837C throw',
        },
        'handler_args': {
            'layer': 'arg0：必须是 int 且在 0..0x1F，否则整条命令忽略（0x43C834/0x43C840）',
            'w': 'arg1：缺省 8；只有"是 int 且 >= 0"才覆盖（0x43C858/0x43C861）',
            'h': 'arg2：缺省 8；只有"是 int 且 >= 0"才覆盖（0x43C85D/0x43C870）',
        },
        'on_ok': {
            'panel_w': 'tw+0x5dc',
            'panel_h': 'tw+0x5de',
            'surface': '0x4289f0（建/改 surface）',
            'buffer': 'tw+0x520 = 分配(w*h*4)',
        },
        'conclusion': 'w > 1024 或 h > 1024 时原版**抛异常**而不是静默放弃；'
                      '本引擎（gfx_text_set_buff）在同一条件下返回 0（放弃），'
                      '因为 VM 侧没有异常语义可承载。'
                      '脚本的正文层调用 TextBuff(0,1380,1024) 正好越界，'
                      '所以那段窗口设置在原版里会中断——不是我们引擎的 bug。',
        'instructions': checked,
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 已校验 %d 条关键指令 ==' % len(checked))
    for c in checked[:5]:
        print('  %s  %s' % (c['pc'], c['instr']))
    print('  ...')
    print('上限：w/h 各 0x400；越界 → 抛异常（code 0x80），不是静默放弃')
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