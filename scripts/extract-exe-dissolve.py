#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract-exe-dissolve.py -- 把 `Dissolve(14)` / `DissolveWait(15)` 的状态机抄成基线。

为什么要这个：2026-11-27 的 pctrace 显示，带 gfx 的外壳在 **`0x59264..0x5928C`** 的
循环里空转（不是以前以为的 `0x592DF` 的 `MotionAlphaTest(265)`）。那个循环唯一的出口
是 `DissolveWait(15)` 返回 false；而 exe 的 handler（`0x437520`）对**非空**实参
**不写返回值**，只置线程标志。所以循环能不能出来，取决于进循环时线程 temp 里残留的
值 —— 这是变体比较语义（bool vs int）的问题，不是动画记录的问题。

本脚本冻结三块事实（只含地址与指令文本）：

1. `Dissolve(14)` handler `0x43A010` 写 `engine+0x101010`（状态）3/6 等值；
2. 逐帧 tick `0x436FC0` 推进 `+0x10101C`（已用时）、与 `+0x101018`（总时长）比较，
   结束后把 `+0x101010` 归 0（state==3 时归 1）；
3. 渲染 `0x43703E` 用 `alpha = 0xFF - elapsed*255/duration` 写屏幕淡出图元 `+0x7`。

用法：
    python scripts/extract-exe-dissolve.py            # 打印 + 写 json
    python scripts/extract-exe-dissolve.py --check
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT = os.path.join(ROOT, 'ref', 'exe-dissolve.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

SITES = [
    ('0x43752c', 'cmpl   $0x0,0x101010(%ecx)', 'DissolveWait：状态为 0 时什么都不做'),
    ('0x437540', 'cmpb   $0x0,(%ecx)', 'DissolveWait：实参 type==0（null/false）时……'),
    ('0x437545', 'movb   $0x1,0x814(%eax)', '……只写返回类型 1，不写值'),
    ('0x43754f', 'orl    $0x10,0x808(%eax)', '非空实参：置线程标志 0x10'),
    ('0x437556', 'movb   $0x1,0x810(%eax)', '非空实参：线程 +0x810 = 1（等待）'),
    ('0x436fc1', 'mov    0x101010(%ecx),%esi', 'tick：读状态'),
    ('0x436fd0', 'add    %eax,0x10101c(%ecx)', 'tick：已用时 += dt'),
    ('0x436fe0', 'cmp    0x101018(%ecx),%edx', 'tick：已用时 vs 总时长'),
    ('0x436ff0', 'mov    %eax,0x101010(%ecx)', 'tick：结束把状态归 0（state==3 归 1）'),
    ('0x43704b', 'idivl  0x101018(%ecx)', '渲染：按已用时/总时长算比例'),
    ('0x437054', 'add    $0x100fdc,%ecx', '渲染：屏幕淡出图元'),
    ('0x43705f', 'mov    %dl,0x7(%ecx)', '渲染：alpha 写图元 +0x7'),
    ('0x43a06d', 'movl   $0x3,0x101010(%edi)', 'Dissolve：状态置 3'),
    ('0x43a18c', 'movl   $0x6,0x101010(%edi)', 'Dissolve：状态置 6'),
    ('0x445477', 'mov    $0x1,%ecx', '调度器：ecx = 运行态标志 1'),
    ('0x44547c', 'test   $0x10,%al', '调度器：看线程是否在等淡出（bit4）'),
    ('0x445486', 'cmp    %ecx,0x10146c(%edx)', '调度器：比较"待完成淡出计数"'),
    ('0x44548e', 'and    $0xffffffef,%eax', '调度器：计数 <= 1 时清 bit4（放行）'),
    ('0x44549f', 'movb   $0x0,0x810(%esi)', '调度器：清"阻塞"字节'),
    ('0x4454b0', 'mov    0x81c(%esi),%eax', '调度器：取回 VM pc'),
    ('0x4454d0', 'call   *%eax', '调度器：执行一条指令'),
    ('0x4454d2', 'cmpb   $0x0,0x810(%esi)', '调度器：+0x810 又被置 1 就退出（阻塞）'),
]

# 下面三个是**脚本侧**地址（HCB 字节码，不在 exe 反汇编里），由
# work/dis_menu.py 解出来；冻结在这里是为了让读文档的人能直接对着看。
SCRIPT_LOOP = [
    dict(pc='0x59264', instr='pushfalse (op 0x08)', role='loop 入口：压 **false/null**（类型字节 0）当实参'),
    dict(pc='0x59265', instr='syscall 15 (DissolveWait)', role='唯一的出口判定'),
    dict(pc='0x59268', instr='pushtemp (op 0x14)', role='取回 syscall 返回；先清 +0x814，所以"没写返回"= 类型 0'),
    dict(pc='0x59269', instr='pushtrue (op 0x09)', role='比较对象：**true**（类型字节 1）'),
    dict(pc='0x5926a', instr='eq (op 0x22)', role='矩阵 [7*typeA+typeB]：(1,1)->恒 1；(0,1)->默认 0'),
    dict(pc='0x5926b', instr='jmpcond 0x59291', role='eq 结果为 0（即返回类型不是 1）才跳出循环'),
    dict(pc='0x5928c', instr='jmp 0x59264', role='否则回到 loop 入口'),
]


def load_rows(path):
    rows = {}
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            m = LINE_RE.match(line)
            if m:
                rows[int(m.group(1), 16)] = m.group(3).strip()
    return rows


def build():
    if not os.path.isfile(ASM):
        return None
    rows = load_rows(ASM)
    sites = []
    for pc_s, want, role in SITES:
        got = rows.get(int(pc_s, 16), '')
        sites.append(dict(pc=pc_s, instr=got, expect=want, role=role,
                          ok=got.startswith(want)))
    return dict(
        schema='fvp-dissolve/1',
        source='iroseka_HD.asm（objdump 反汇编）+ pctrace 实测',
        claim=('带 gfx 的外壳卡在 0x59264..0x5928C 的 DissolveWait 循环。这个循环是'
               '**查询式**等待：实参是 `pushfalse`（op 0x08，类型字节 0），handler 走'
               '"查询"分支——状态非 0 时只把返回**类型**写成 1（true），状态为 0 时'
               '什么都不写（类型保持 0 = false）。脚本再拿 `pushtrue` 去 eq：(1,1) 在'
               '矩阵里恒为 1 → 继续循环；(0,1) 用默认 helper 返回 0 → 跳出。所以出口'
               '由**类型字节**决定，与线程 temp 的残留值无关'),
        state=dict(addr='engine+0x101010', idle=0, phases=[1, 2, 3, 4, 5, 6],
                   duration='engine+0x101018', elapsed='engine+0x10101c',
                   fade_prim='engine+0x100fdc', fade_alpha_offset=7),
        wait_modes=dict(
            query=dict(arg='type 0（pushfalse）', state_nonzero='只写 +0x814 = 1（类型 true）',
                       state_zero='什么都不写 → 类型保持 0 = false',
                       use='0x59264 的轮询：`while (DissolveWait(false))`'),
            wait=dict(arg='type != 0（pushtrue 等）',
                      action='+0x808 |= 0x10、+0x810 = 1（挂起线程，等淡出结束）',
                      use='真正"等淡出播完"的用法'),
            note='两个分支都**不写 +0x818**（返回值载荷）；查询分支的语义完全由 +0x814 '
                 '的类型字节承载'),
        loop=dict(region=['0x59264', '0x5928c'],
                  exit='DissolveWait(15) 返回的类型字节为 0（即淡出状态 engine+0x101010 == 0）',
                  not_the_265_loop='0x592DF 的 MotionAlphaTest(265) 在这段之后',
                  script=SCRIPT_LOOP),
        scheduler=dict(
            fn='0x445440',
            wait_flag='thread+0x808 bit4 (0x10)',
            blocked_byte='thread+0x810',
            release_counter='engine+0x10146c',
            release_rule='engine+0x10146c <= 1 时清 bit4 并继续执行',
            writes_return_value=False,
            note=('线程被放行时只清 bit4 与 +0x810，**不写 +0x818**；DissolveWait 的两个'
                  '分支也都不写载荷。所以 eq 实际只比较**类型字节**（见 wait_modes），'
                  '放行与否不改变比较结果')),
        sites=sites)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    a = ap.parse_args(argv)
    payload = build()
    if payload is None:
        print('SKIP: %s not found' % ASM)
        return 0
    bad = [s for s in payload['sites'] if not s['ok']]
    for s in payload['sites']:
        print('%s %-5s %-34s %s' % (s['pc'], 'ok' if s['ok'] else 'DIFF',
                                    s['instr'][:34], s['role']))
    if a.check:
        return 0 if not bad else 1
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(payload, f, ensure_ascii=False, indent=1)
    print('json -> %s' % OUT)
    return 0 if not bad else 1


if __name__ == '__main__':
    sys.exit(main())