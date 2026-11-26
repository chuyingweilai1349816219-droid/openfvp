#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**图元 setter handler 的实参规则**（A 类：ref/exe-prim-setters-args.json）。

本轮精读 0x43BACA..0x43BFDA 里的四个 handler（上一轮的扫描已锁定这一小段）：

  0x43BA90  写 prim+0x20 / +0x22（op1/op2），逐分量 flags |= 0x02，末尾 flags |= 0x40
  0x43BAF0  angle = v % 3600（0xE10，负数归一），scale 钳位 100..10000 → +0x26/+0x28
  0x43BC40  造 type-7 图元（0x439BF0(id,7)），arg1 存 +0x2E，字段全复位后再写 x/y
  0x43BD00  同 0x43BC40，但 type 4、arg1 范围 -2..0xFFF

每个 handler 的公共前置：arg0 必须是 int 且在 1..0xFFF（图元 id 范围）。

用法：
    python scripts/extract-exe-prim-setters-args.py            # 打印 + 写 json
    python scripts/extract-exe-prim-setters-args.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-prim-setters-args.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

EXPECT = [
    # 0x43d6d0：PrimGroupIn（cmd 73）：child 1..0xFFF、parent 0..0xFFF
    (0x43D6D4, 'cmpb   $0x2,(%eax)'),
    (0x43D6DC, 'cmp    $0x1,%edx'),
    (0x43D6E1, 'cmp    $0x1000,%edx'),
    (0x43D6E9, 'cmpb   $0x2,0x8(%eax)'),
    (0x43D6F2, 'test   %eax,%eax'),
    (0x43D6F6, 'cmp    $0x1000,%eax'),
    (0x43D6FF, 'call   0x43d640'),
    # 0x43ace0：MotionMoveTest（cmd 50）：id 1..0xFFF -> 线程 +0x814（1 = 还在跑）
    (0x43ACE4, 'cmpb   $0x2,(%eax)'),
    (0x43ACEC, 'cmp    $0x1,%eax'),
    (0x43ACF1, 'cmp    $0xfff,%eax'),
    (0x43AD13, 'call   0x4231a0'),
    (0x43AD1A, 'setne  %al'),
    (0x43AD1D, 'mov    %al,0x814(%esi)'),
    # 0x437990：PrimSetAlpha（cmd 77）：id 1..0xFFF、alpha 0..0xFF -> prim+0x07
    (0x437994, 'cmpb   $0x2,(%edx)'),
    (0x43799C, 'cmp    $0x1,%eax'),
    (0x4379A1, 'cmp    $0x1000,%eax'),
    (0x4379AE, 'cmpl   $0xff,0xc(%edx)'),
    (0x4379BA, 'imul   $0x34,%eax,%eax'),
    (0x4379BD, 'mov    %dl,0x61dd6b(%eax,%ecx,1)'),
    # 0x4379d0：PrimSetDraw（cmd 79）：id 0..0xFFF、draw 只收 0/1 -> prim+0x05
    (0x4379DC, 'test   %eax,%eax'),
    (0x4379E7, 'cmpb   $0x2,0x8(%edx)'),
    (0x4379F0, 'test   %edx,%edx'),
    (0x437A01, 'cmp    $0x1,%edx'),
    (0x437A09, 'mov    %dl,0x61dd69(%eax,%ecx,1)'),
    # 0x43ba20：PrimSetBlend（cmd 78）：id 0..0xFFF，blend 0..1 -> prim+0x08
    (0x43BA24, 'cmpb   $0x2,(%edx)'),
    (0x43BA2E, 'jl     0x43ba58'),
    (0x43BA45, 'cmp    $0x1,%esi'),
    (0x43BA4D, 'imul   $0x34,%eax,%eax'),
    (0x43BA50, 'mov    %dl,0x61dd6c(%eax,%ecx,1)'),
    (0x43BA97, 'cmp    %bl,(%edx)'),
    (0x43BAA3, 'cmp    $0xfff,%eax'),
    (0x43BAAD, 'cmpb   $0x4,0x61dd68(%eax,%ecx,1)'),
    (0x43BAC7, 'or     %bl,0x6(%eax)'),
    (0x43BACA, 'mov    %cx,0x20(%eax)'),
    (0x43BADA, 'mov    %dx,0x22(%eax)'),
    (0x43BADE, 'orb    $0x40,0x6(%eax)'),
    (0x43BB1E, 'mov    $0xe10,%edi'),
    (0x43BB23, 'idiv   %edi'),
    (0x43BB34, 'mov    %dx,0x24(%ecx)'),
    (0x43BB41, 'cmp    $0x64,%eax'),
    (0x43BB5B, 'cmp    $0x2710,%eax'),
    (0x43BB4F, 'mov    %ax,0x26(%ecx)'),
    (0x43BB53, 'mov    %ax,0x28(%ecx)'),
    (0x43BC7B, 'push   $0x7'),
    (0x43BC7E, 'call   0x439bf0'),
    (0x43BC85, 'movb   $0x0,0x6(%eax)'),
    (0x43BC8D, 'mov    %dx,0x14(%eax)'),
    (0x43BC9C, 'mov    %cx,0x2e(%eax)'),
    (0x43BCB7, 'mov    %cx,0x1c(%eax)'),
    (0x43BCC9, 'mov    %dx,0x26(%eax)'),
    (0x43BCD1, 'movb   $0xff,0x7(%eax)'),
    (0x43BD3F, 'push   $0x4'),
    (0x43BD42, 'call   0x439bf0'),
    (0x43BD32, 'cmp    $0xfffffffe,%edx'),
    (0x43BD54, 'xor    %edx,%edx'),
    # 0x43bdc0：造 type-5（文本）图元，层号 0..0x1F -> +0x32
    (0x43BDD9, 'push   $0x5'),
    (0x43BDDC, 'call   0x439bf0'),
    (0x43BDE1, 'andb   $0xfe,0x6(%eax)'),
    (0x43BDF1, 'mov    %cx,0x16(%eax)'),
    (0x43BE09, 'cmp    $0x1f,%ecx'),
    (0x43BE0E, 'mov    %cx,%dx'),
    (0x43BE11, 'mov    %dx,0x32(%eax)'),
    # 0x43be40：造 type-2 图元，+0x30 范围 -1..0xFF，再写 x/y/w/h
    (0x43BE66, 'push   %ebx'),
    (0x43BE68, 'call   0x439bf0'),
    (0x43BE72, 'mov    %cx,0x30(%eax)'),
    (0x43BE99, 'cmp    $0xffffffff,%ecx'),
    (0x43BE9E, 'cmp    $0xff,%ecx'),
    (0x43BED0, 'mov    %cx,0x18(%eax)'),
    (0x43BEDD, 'mov    %dx,0x1a(%eax)'),
    # 0x43bef0：u/v 对（+0x1C/+0x1E），逐分量 flags |= 0x01
    (0x43BF26, 'orb    $0x1,0x6(%eax)'),
    (0x43BF2A, 'mov    %cx,0x1c(%eax)'),
    (0x43BF3C, 'mov    %dx,0x1e(%eax)'),
    # 0x43bf50：w/h（+0x18/+0x1A），逐分量 flags |= 0x01
    (0x43BF7C, 'orb    $0x1,0x6(%eax)'),
    (0x43BF80, 'mov    %cx,0x18(%eax)'),
    (0x43BF92, 'mov    %dx,0x1a(%eax)'),
    # 0x43bfa0：x/y（+0x14/+0x16），不置"已写"位，末尾 flags |= 0x40
    (0x43BFCC, 'mov    %cx,0x14(%eax)'),
    (0x43BFDA, 'mov    %dx,0x16(%eax)'),
    (0x43BFDE, 'orb    $0x40,0x6(%eax)'),
]

HANDLERS = [
    {'handler': '0x43d6d0', 'confirmed_id': 73,
     'writes': ['prim 树父子关系（0x43D640(parent, child)）'],
     'rules': ['arg0 child = int 且 **1..0xFFF**',
               'arg1 parent = int 且 **0..0xFFF**（允许 0）',
               '两个都合法才挂接，否则整条忽略；不置 flags'],
     'guess': 'PrimGroupIn(73)（两部作品合计 349 次）'},
    {'handler': '0x43ace0', 'confirmed_id': 50,
     'writes': ['线程 +0x814（VM 返回值）'],
     'rules': ['arg0 = int 且 **1..0xFFF**',
               '`0x4231A0(engine+0x65BA54, id)` 查该图元动作是否还在跑',
               '`setne` 后**无条件**写线程 +0x814：1 = 还在跑，0 = 已结束'],
     'guess': 'MotionMoveTest(50)（两部作品合计 447 次）'},
    {'handler': '0x43a8b0', 'confirmed_id': 42,
     'writes': ['tail-call 0x42d460(engine+0x651E04, id, from, to, ms, mode, flag)'],
     'rules': ['arg0 = int 1..0xFFF',
               'arg1 起始 alpha：int 且 0..0xFF，缺/越界回退成**当前 prim+0x07**',
               'arg2 目标 alpha：同上',
               'arg3 时长 ms：int 且 **1..300000**，不合法则整条忽略',
               'arg4 mode：int 0..1（缺省 0）',
               'arg5 flag：任意类型，`type != 0` → 1'],
     'guess': 'MotionAlpha(42)（两部作品合计 553 次）'},
    {'handler': '0x43b9c0', 'confirmed_id': 76,
     'writes': ['线程 +0x814（VM 返回值）'],
     'rules': ['arg0 = int 1..0xFFF',
               'arg1：`type != 0` → 1，否则 0（布尔）',
               '`0x43B8D0(id, flag)` 命中才把线程 +0x814 置 1',
               '未命中时不写（保持旧值）'],
     'guess': 'PrimHit(76)（389 次；鼠标命中测试）'},
    {'handler': '0x437990', 'confirmed_id': 77,
     'writes': ['prim+0x07 = alpha'],
     'rules': ['arg0 = int 且 **1..0xFFF**',
               'arg1 int 且 **0..0xFF**（`cmpl $0xff; ja` 无符号比较），越界保持原值',
               '不置任何 flags 位'],
     'guess': 'PrimSetAlpha(77)（两部作品合计 1052 次，全片最高频）'},
    {'handler': '0x4379d0', 'confirmed_id': 79,
     'writes': ['prim+0x05 = draw（可见性）'],
     'rules': ['arg0 = int 且 **0..0xFFF**（这一条允许 0）',
               'arg1 int 且**只接受 0 或 1**，其它值保持原值',
               '不置 flags；绘制分派先判 prim+0x05 == 0 直接返回（0x438AB5）'],
     'guess': 'PrimSetDraw(79)（两部作品合计 353 次）'},
    {'handler': '0x43ba20', 'confirmed_id': 78,
     'writes': ['prim+0x08 = blend'],
     'rules': ['arg0 = int 且 **0..0xFFF**（这一条允许 0）',
               'arg1 int 且 **0..1** → prim+0x08 = (u8)值',
               '越界保持原值；不置任何 flags 位'],
     'guess': 'PrimSetBlend(78)', 'confirmed_id': 78},
    {'handler': '0x43ba90', 'confirmed_id': 81, 'writes': ['prim+0x20 = op1', 'prim+0x22 = op2'],
     'rules': ['arg0 = int 且 1..0xFFF（图元 id）',
               '该图元 prim+0x04 必须是 4（类型检查）',
               'arg1/arg2 各自 int 才写，写 u16 并 flags |= 0x02',
               '末尾无条件 flags |= 0x40'],
     'guess': 'PrimSetOP(81)'},
    {'handler': '0x43baf0', 'confirmed_id': 82, 'writes': ['prim+0x24 = angle', 'prim+0x26/0x28 = scale'],
     'rules': ['arg0 = int 且 1..0xFFF',
               'arg1 int → angle = v % 3600（0xE10），负数也归一化，存 u16',
               'arg2 int → scale 钳位 100..10000（per-mille），同时写 +0x26 与 +0x28',
               '每个已写分量都 flags |= 0x40'],
     'guess': 'PrimSetRS(82)'},
    {'handler': '0x43bc40', 'confirmed_id': 84, 'writes': ['prim+0x2E = arg1', 'prim+0x14/0x16 = x/y'],
     'rules': ['arg0 = int 且 1..0xFFF',
               '0x439BF0(id, 7) 造 type-7 图元',
               '字段全复位：flags=0、x/y/w/h/op=0、+0x12=1000、scale=1000/1000、'
               'alpha(0x07)=0xFF、+0x08=0',
               'arg1 int 且 0..1 → +0x2E',
               'arg2/arg3 int → x/y（逐分量）'],
     'guess': 'PrimSetSnow(84)（type 7 = snow）'},
    {'handler': '0x43bdc0', 'confirmed_id': 86, 'writes': ['prim+0x32 = text layer', 'prim+0x14/0x16 = x/y'],
     'rules': ['arg0 = int 且 1..0xFFF',
               '0x439BF0(id, 5) 造 **type-5（文本）**图元',
               'flags &= 0xFE（清"尺寸已写"位）',
               '+0x32=0、x/y=0、alpha(0x07)=0xFF、+0x08=0',
               'arg1 int 且 **0..0x1F** → +0x32（文本层号，与 TextBuff 的层号范围一致）',
               'arg2/arg3 int → x/y（逐分量）'],
     'guess': 'PrimSetText(86)（type 5 = text）'},
    {'handler': '0x43be40', 'confirmed_id': 87, 'writes': ['prim+0x30 = arg1', 'x/y/w/h'],
     'rules': ['arg0 = int 且 1..0xFFF',
               '0x439BF0(id, 2) 造 **type-2** 图元',
               '+0x30=0xFF、x/y/w/h=0、alpha=0xFF、+0x08=0',
               'arg1 int 且 **-1..0xFF** → +0x30',
               'arg2/arg3/arg4/arg5 int → x/y/w/h（逐分量）'],
     'guess': 'PrimSetTile(87)（type 2 = tile）'},
    {'handler': '0x43bef0', 'confirmed_id': 88, 'writes': ['prim+0x1C = u', 'prim+0x1E = v'],
     'rules': ['arg0 = int 且 1..0xFFF，且该图元 prim+4 必须是 4（类型检查）',
               'arg1/arg2 各自 int → u16，逐分量 flags |= 0x01',
               '末尾无条件 flags |= 0x40'],
     'guess': 'PrimSetUV(88)'},
    {'handler': '0x43bf50', 'confirmed_id': 89, 'writes': ['prim+0x18 = w', 'prim+0x1A = h'],
     'rules': ['arg0 = int 且 1..0xFFF（无类型检查）',
               'arg1/arg2 各自 int → u16，逐分量 flags |= 0x01',
               '末尾无条件 flags |= 0x40'],
     'guess': 'PrimSetWH(89)'},
    {'handler': '0x43bfa0', 'confirmed_id': 90, 'writes': ['prim+0x14 = x', 'prim+0x16 = y'],
     'rules': ['arg0 = int 且 1..0xFFF（无类型检查）',
               'arg1/arg2 各自 int → u16（**不置任何"已写"位**）',
               '末尾无条件 flags |= 0x40'],
     'guess': 'PrimSetXY(90)'},
    {'handler': '0x43bd00', 'confirmed_id': 85, 'writes': ['prim+0x2E = arg1', 'prim+0x14/0x16 = x/y'],
     'rules': ['arg0 = int 且 1..0xFFF',
               '0x439BF0(id, 4) 造 type-4 图元',
               '字段全复位（同 0x43bc40）',
               'arg1 int 且 -2..0xFFF → +0x2E',
               'arg2/arg3 int → x/y（逐分量）'],
     'guess': 'PrimSetSprt(85)（type 4 = sprite）'},
]


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
    checked = []
    for addr, want in EXPECT:
        got = rows.get(addr)
        if got != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        checked.append({'pc': '0x%06x' % addr, 'instr': want})

    result = {
        'note': '图元 setter handler 的实参规则（A 类，逐条带指令）；'
                '由 scripts/extract-exe-prim-setters-args.py 生成',
        'block': '0x43baca..0x43bfda（上一轮全量扫描锁定的区域）',
        'common': ['arg0 必须是 int 且 1..0xFFF（图元 id 范围）',
                   '建立/查询图元时用 engine+0x61DD64 + id*0x34',
                   '每个"已写入"的分量都会把 prim+0x06 的某个位置起来，'
                   '0x40 位表示"这个图元被改过"'],
        'handlers': HANDLERS,
        'instructions': checked,
        'unknown': ['PrimSetBlend(78)/PrimSetOP(81)/PrimSetWH(89)/PrimSetXY(90) 的 id'
                    '已由 ref/exe-cmd-dispatch.json 证实（见 confirmed_id）；'
                    'handler 与命令 id 的对应已由 ref/exe-cmd-dispatch.json 证实'
                    '（81/89/90 = 0x43BA90/0x43BF50/0x43BFA0、78 = 0x43BA20）；'
                    '下面 guess 字段保留最初读代码时的猜测，供对照',
                    'prim+0x06 的 0x40 位在渲染端如何使用还没读',
                    'handler 0x43BE40 造 type-2 图元时 +0x30 的语义（1..0xFF 范围）'],
        'c_side': {
            'fixed_205': 'case 84（PrimSetSnow）：造 type-7 图元 + 位置 + 0..1 开关，'
                         '与原版 0x43BC40 的结构一致（渲染端仍不画 snow）',
            'fixed_199': 'src/syscall.c 的 case 81/89/90 现在按原版把值当 **int16** 存'
                         '（FVP_S16：u16 截断），因为原版这些字段都是 u16 写、int16 读',
            'fixed_198': 'case 82（PrimSetRS）：angle 取模 3600、scale 钳位 100..10000',
            'already_ok': 'case 81/89/90 的逐分量 int 检查（非 int = 保持原值）',
            'note': 'prim+0x06 的"已写"位（bit0/bit1）我们只在语义上跟随，'
                    '渲染器不依赖它们',
        },
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 四个 handler ==')
    for h in HANDLERS:
        print('  %s  %-18s %s' % (h['handler'], h['guess'],
                                  ' / '.join(h['rules'])[:70]))
    print('已校验 %d 条指令' % len(checked))
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