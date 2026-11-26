#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 的 handler 原版程序里提取 Motion*/PrimSet* 的**实参约束**（A 类事实）。

为什么需要它：脚本侧只能看到"调了什么"，看不到"参数的单位/取值范围/缺省语义"。
这些全在 exe 的 handler 里：每个实参先做类型检查（`cmpb $0x2,(%eax)`）与范围检查
（成对的 `cmp $MIN` / `cmp $MAX`），越界就整条命令丢弃。把它们读出来，就能：

  * 确认单位（例如缩放 100..10000 = per-mille，1.0 = 1000）；
  * 确认时长与缓动模式的范围（MotionMoveS2 的 mode **1..5** —— 与 ④ 缓动目标对齐）；
  * 确认"某个实参不是整数时沿用当前值"这类缺省语义；
  * 确认 `...Stop` 到底是"取消动画（保持当前值）"还是"回到起点"。

本脚本不猜：只把 handler 里的**常量**与**访问偏移**抄出来。

产物 `ref/exe-motion-params.json`（只含参数，不含游戏数据）。

用法：
    python scripts/extract-exe-motion-params.py            # 打印 + 写 json
    python scripts/extract-exe-motion-params.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
HANDLERS = os.path.join(ROOT, 'work', 'vm_id_handler.json')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-motion-params.json')

# 关注这几条：它们的语义直接决定标题/动画怎么摆
# 有些 handler 用寄存器比较类型（`cmp %cl,(%eax)`），正则归因不到 arg 下标；
# 这些命令的实参布局由人工按反汇编补齐，写进 ARG_LAYOUT。
ARG_LAYOUT = {
    48: [
        {'arg': 1, 'role': 'from_x', 'default': '当前 prim+0x14'},
        {'arg': 2, 'role': 'from_y', 'default': '当前 prim+0x16'},
        {'arg': 3, 'role': 'to_x', 'default': '当前 prim+0x14'},
        {'arg': 4, 'role': 'to_y', 'default': '当前 prim+0x16'},
        {'arg': 5, 'role': 'ms', 'range': [1, 300000]},
        {'arg': 6, 'role': 'mode', 'range': [1, 5]},
        {'arg': 7, 'role': 'flag', 'type': '任意，type != 0 -> 1'},
    ],
}

TARGETS = {
    42: 'MotionAlpha',
    48: 'MotionMove',
    54: 'MotionMoveS2',
    55: 'MotionMoveS2Stop',
    77: 'PrimSetAlpha',
    78: 'PrimSetBlend',
    81: 'PrimSetOP',
    82: 'PrimSetRS',
    89: 'PrimSetWH',
    90: 'PrimSetXY',
    91: 'PrimSetZ',
}

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
# Variant 数组按 8 字节步进：类型在 arg*8，值在 arg*8+4
TYPE_RE = re.compile(r'cmpb\s+\$0x([0-9a-f]+),(?:0x([0-9a-f]+))?\(%e([abcd]x|si|di)\)')
TEST_RE = re.compile(r'^test\s+%e(\w+),%e\1$')
CMP_RE = re.compile(r'^cmp\s+\$0x([0-9a-f]+),%e(dx|ax|di|si|bx|bp)$')
STORE_RE = re.compile(r'mov\s+%e?\w+,0x([0-9a-f]+)\(%e?[abcd]x,%e?[abcd]x(?:,1)?\)')
CALL_RE = re.compile(r'^call\s+0x([0-9a-f]+)$')
CLAMP_RE = re.compile(r'^cmp\s+\$0x([0-9a-f]+),%e([abcd]x|di|si|bp)$')
IDIV_RE = re.compile(r'^idiv\s+%e(\w+)$')
DIVSET_RE = re.compile(r'^mov\s+\$0x([0-9a-f]+),%edi$')
FIELD_RE = re.compile(r'^mov\s+%([a-z]{2}),0x([0-9a-f]+)\(%e([abcd]x)\)$')
# PrimSetOP 只对 type==4 的图元生效：cmpb $0x4,0x61dd68(%eax,%ecx,1)
PRIM_TYPE_RE = re.compile(r'cmpb\s+\$0x([0-9a-f]+),0x61dd6[0-9a-f]+\(%e?[abcd]x,%e?[abcd]x,1\)')
# 绝对寻址的字段写：mov %dl,0x61dd6b(%eax,%ecx,1) —— 基准 0x61dd64，低字节差即字段偏移
ABS_FIELD_RE = re.compile(r'^mov\s+%([a-z]{2}),0x(61dd[0-9a-f]{2})\(%e[abcd]x,%e[abcd]x,1\)$')
# 脏标记：orb $0x1,0x6(%eax) —— prim+6 是 dirty 位图
DIRTY_RE = re.compile(r'^orb\s+\$0x([0-9a-f]+),0x6\(%e[abcd]x\)$')
JMP_RE = re.compile(r'^j(?:mp)?\s+0x([0-9a-f]+)$')


def load_asm(path):
    rows = []
    with open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def handler_window(rows, start, max_instr=90):
    out = []
    for addr, text in rows:
        if addr < start:
            continue
        out.append((addr, text))
        if text.startswith('ret') or len(out) >= max_instr:
            break
    return out


def extract(win):
    """从 handler 窗口里抽：类型检查（arg 下标 + 要求类型）、范围检查、跳转目标。"""
    types = []
    ranges = []
    tails = []
    dividers = []        # idiv 的除数（角度 3600 这种单位常量）
    fields = []          # 写进 prim 结构体的字段偏移
    abs_fields = []      # 绝对寻址的字段写（PrimSetAlpha 的 +7 这种）
    dirty = []           # prim+6 的脏标记位
    gates = []           # 图元类型门（例如 PrimSetOP 只对 type==4 生效）
    cond = []            # 条件写入：arg 是 int 才写某个字段（非 int = 保持原值）
    pending = None       # 最近的 cmp $MIN 值
    for addr, text in win:
        m = TYPE_RE.search(text)
        if m:
            want = int(m.group(1), 16)
            off = int(m.group(2), 16) if m.group(2) else 0
            types.append({'arg': off // 8, 'type': want})
            continue
        m = TEST_RE.match(text)
        if m:
            pending = 0        # test/jl => 下界 0
            continue
        m = CMP_RE.match(text)
        if m:
            val = int(m.group(1), 16)
            if pending is None:
                pending = val
            else:
                lo, hi = min(pending, val), max(pending, val)
                # 最近的类型检查决定这是哪个实参
                arg = types[-1]['arg'] if types else None
                ranges.append({'arg': arg, 'min': lo, 'max': hi})
                pending = None
            continue
        m = CALL_RE.match(text)
        if m:
            tails.append('call 0x%08x' % int(m.group(1), 16))
            continue
        m = JMP_RE.match(text)
        if m:
            tails.append('jmp 0x%08x' % int(m.group(1), 16))
            continue
        m = DIVSET_RE.match(text)
        if m:
            dividers.append(int(m.group(1), 16))
            continue
        m = FIELD_RE.match(text)
        if m:
            fields.append({'field_off': int(m.group(2), 16), 'reg': m.group(1),
                           'base': m.group(3)})
            continue
        m = ABS_FIELD_RE.match(text)
        if m:
            abs_fields.append({'field_off': int(m.group(2), 16) - 0x61DD64,
                               'reg': m.group(1)})
            continue
        m = DIRTY_RE.match(text)
        if m:
            dirty.append(int(m.group(1), 16))
            continue
        m = PRIM_TYPE_RE.search(text)
        if m:
            gates.append({'kind': 'prim_type', 'value': int(m.group(1), 16),
                          'source': text})
            continue
    # 条件写入：TypeCheck(arg) 之后 6 条内出现的 (%eax) 字段写
    pending_arg = None
    for k, (addr, text) in enumerate(win):
        m = TYPE_RE.search(text)
        if m:
            off = int(m.group(2), 16) if m.group(2) else 0
            pending_arg = (off // 8, k)
            continue
        m = FIELD_RE.match(text)
        if m and pending_arg is not None and m.group(3) in ('ax', 'cx') \
                and k - pending_arg[1] <= 6:
            cond.append({'arg': pending_arg[0], 'field_off': int(m.group(2), 16),
                         'only_if_int': True})
            pending_arg = None
    return {'arg_types': types, 'arg_ranges': ranges, 'calls_or_jumps': tails,
            'split_constants': sorted(set(dividers)),
            'struct_field_writes': fields,
            'abs_field_writes': abs_fields,
            'dirty_flag_bits': sorted(set(dirty)),
            'type_gates': gates,
            'conditional_field_writes': cond}


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM) or not os.path.isfile(HANDLERS):
        print('SKIP: 缺原版程序清单或 handler 表')
        return 0

    with io.open(HANDLERS, encoding='utf-8') as fp:
        table = {e['id']: e for e in json.load(fp)}
    rows = load_asm(ASM)

    result = {'note': '从 exe handler 静态分析提取的实参约束（A 类事实）；'
                      '由 scripts/extract-exe-motion-params.py 生成', 'commands': {}}
    for cid, name in TARGETS.items():
        e = table.get(cid)
        if e is None:
            continue
        win = handler_window(rows, e['handler'])
        info = extract(win)
        info.update({'id': cid, 'name': name, 'argc': e['argc'],
                     'handler': '0x%08x' % e['handler'],
                     'reg_index': e.get('reg_index')})
        if cid in ARG_LAYOUT:
            info['arg_layout'] = ARG_LAYOUT[cid]
        result['commands'][str(cid)] = info
        print('%3d %-16s argc=%d handler=0x%08x' % (cid, name, e['argc'],
                                                    e['handler']))
        for t in info['arg_types']:
            print('     arg%d 必须是 int(type=%d)' % (t['arg'], t['type']))
        for r in info['arg_ranges']:
            print('     arg%s 范围 0x%X..0x%X (%d..%d)'
                  % (r['arg'], r['min'], r['max'], r['min'], r['max']))
        for f in info['abs_field_writes']:
            print('     绝对寻址写入 prim+0x%X' % f['field_off'])
        if info['dirty_flag_bits']:
            print('     prim+6 脏标记 %s'
                  % ' '.join('0x%X' % b for b in info['dirty_flag_bits']))
        for t in info['calls_or_jumps']:
            print('     %s' % t)

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 与 %s 不一致' % OUT_JSON)
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/exe-motion-params.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())