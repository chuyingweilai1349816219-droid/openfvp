#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 的 handler 原版程序里抄出**文本样式命令**的字段与取值范围（A 类事实）。

为什么需要它：`TextFormat`(118) / `TextFunction`(119) / `TextShadowDist`(125) /
`TextSkip`(127) / `TextSpace`(128) / `TextFont`(113) 在我们的引擎里一直是桩。
要知道它们"到底改哪个字段、合法范围是多少、传非整数是不是保持原值"，正确证据是
exe 的 handler，不是截图。

形态（这几个 handler 高度一致，都是 __thiscall 风格）：

```asm
mov  0x4(%esp),%eax        ; eax = Variant 参数数组
cmpb $0x2,(%eax)           ; arg0 必须是 int（type 2）
mov  0x4(%eax),%ecx        ; arg0 = 文本层号
cmp  $0x1f,%ecx  / jg      ; 层号 0..31
mov  0x481368,%edx         ; edx = 全局设置对象
mov  0x98(%edx,%ecx,4),%ecx; ecx = 设置对象->文本层表[层号]（每层一个样式对象）
cmpb $0x2,0x8(%eax)        ; arg1 必须是 int
mov  0xc(%eax),%edx        ; arg1 的值
cmp  $0xffffffe0,%edx / jl ; 越界就跳过（= 保持当前值）
cmp  $0x20,%edx / jg
mov  %dx,0x5a8(%ecx)       ; 写入样式对象 +0x5A8（16 位）
```

本脚本用一个小数据流把这些抄成结构化的（字段偏移、宽度、值来源实参、取值范围、
越界行为），不做任何推测：**只有真的出现在指令里的常量/偏移才会进产物**。

产物 `ref/exe-text-params.json`（只含参数，不含游戏数据）。

用法：
    python scripts/extract-exe-text-params.py            # 打印 + 写 json
    python scripts/extract-exe-text-params.py --check    # 只比对（门禁用）
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
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-params.json')

TARGETS = {
    113: 'TextFont',
    118: 'TextFormat',
    119: 'TextFunction',
    120: 'TextOutSize',
    125: 'TextShadowDist',
    126: 'TextSize',
    127: 'TextSkip',
    128: 'TextSpace',
    129: 'TextSpeed',
}

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
MOVIMM_RE = re.compile(r'^mov\s+\$0x([0-9a-f]+),%e([a-z]{2})$')
MOVIMM8_RE = re.compile(r'^mov\s+\$0x([0-9a-f]+),%([a-d])l$')
MOVREG_RE = re.compile(r'^mov\s+%e([a-z]{2}),%e([a-z]{2})$')
MOVREG8_RE = re.compile(r'^mov\s+%([a-d])l,%([a-d])l$')
MOVREG16_RE = re.compile(r'^mov\s+%([a-d]x|si|di),%([a-d]x|si|di)$')
CMPREG_RE = re.compile(r'^cmp\s+%e([a-z]{2}),%e([a-z]{2})$')
XOR_RE = re.compile(r'^xor\s+%e([a-z]{2}),%e\1$')
GLOBAL_RE = re.compile(r'^mov\s+0x([0-9a-f]+),%e([a-z]{2})$')
# mov 0x98(%edx,%ecx,4),%ecx —— 文本层表（每层 4 字节指针）
LAYER_RE = re.compile(
    r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2}),%e([a-z]{2}),4\),%e([a-z]{2})$')
ARGFIELD_RE = re.compile(r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
TYPE8_RE = re.compile(r'^cmpb\s+\$0x([0-9a-f]+),0x([0-9a-f]+)\(%e([a-z]{2})\)$')
TYPE8R_RE = re.compile(r'^cmp\s+%([a-d])l,0x([0-9a-f]+)\(%e([a-z]{2})\)$')
TYPE8Z_RE = re.compile(r'^cmpl\s+\$0x([0-9a-f]+),0x([0-9a-f]+)\(%e([a-z]{2})\)$')
CMPI_RE = re.compile(r'^cmp\s+\$0x([0-9a-f]+),%e([a-z]{2})$')
TEST_RE = re.compile(r'^test\s+%e([a-z]{2}),%e\1$')
WRITE16_RE = re.compile(
    r'^mov\s+%([a-d]x|si|di),0x([0-9a-f]+)\(%e([a-z]{2})\)$')
WRITE8_RE = re.compile(r'^mov\s+%([a-d])l,0x([0-9a-f]+)\(%e([a-z]{2})\)$')
JMP_RE = re.compile(r'^jmp\s+0x([0-9a-f]+)$')
CALL_RE = re.compile(r'^call\s+0x([0-9a-f]+)$')


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def window(rows, start, max_instr=140):
    out = []
    for addr, text in rows:
        if addr < start:
            continue
        out.append((addr, text))
        if text.startswith('ret') or len(out) >= max_instr:
            break
    return out


def s32(v):
    return v - 0x100000000 if v >= 0x80000000 else v


def extract(win):
    """迷你数据流：跟踪"哪个寄存器装着第几个实参的值 / 样式对象指针"。"""
    regs = {}          # reg -> ('imm', v) / ('arg', n) / ('styler',) / ('global',)
    bregs = {}         # byte reg -> imm
    checks = []        # 类型检查：arg -> type
    cmps = {}          # arg -> [立即数...]
    tests = set()      # 出现过 test reg,reg 的实参
    writes = []        # {arg, offset, size}
    layer_table = None
    arg_regs = {}      # reg -> 曾装过的实参集合（钳位检测）
    register_cmps = []  # 与寄存器比较的上界（动态范围，例如字体数-1）
    imm_writes = []     # mov $imm,%reg / xor %reg,%reg（含钳位常量）
    globals_ = set()
    tails = []

    for addr, text in win:
        m = MOVIMM_RE.match(text)
        if m:
            regs[m.group(2)] = ('imm', int(m.group(1), 16))
            imm_writes.append({'pc': '0x%x' % addr, 'reg': m.group(2),
                               'imm': int(m.group(1), 16)})
            continue
        m = MOVIMM8_RE.match(text)
        if m:
            bregs[m.group(2)] = int(m.group(1), 16)
            regs[m.group(2) + 'x'] = ('imm', int(m.group(1), 16))
            imm_writes.append({'pc': '0x%x' % addr,
                               'reg': m.group(2) + 'x',
                               'imm': int(m.group(1), 16)})
            continue
        m = XOR_RE.match(text)
        if m:
            regs[m.group(1)] = ('imm', 0)
            arg_regs.setdefault(m.group(1), set())
            imm_writes.append({'pc': '0x%x' % addr, 'reg': m.group(1),
                               'imm': 0})
            continue
        m = MOVREG_RE.match(text)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            continue
        m = MOVREG8_RE.match(text)
        if m:
            regs[m.group(2) + 'x'] = regs.get(m.group(1) + 'x')
            continue
        m = MOVREG16_RE.match(text)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            continue
        m = CMPREG_RE.match(text)
        if m:
            register_cmps.append(text)
            continue
        m = GLOBAL_RE.match(text)
        if m:
            globals_.add(int(m.group(1), 16))
            regs[m.group(2)] = ('global',)
            continue
        m = LAYER_RE.match(text)
        if m:
            layer_table = {'offset': int(m.group(1), 16), 'stride': 4}
            regs[m.group(4)] = ('styler',)
            continue
        m = ARGFIELD_RE.match(text)
        if m:
            arg = int(m.group(1), 16) // 8
            regs[m.group(3)] = ('arg', arg)
            arg_regs.setdefault(m.group(3), set()).add(arg)
            continue
        m = TYPE8_RE.match(text)
        if m:
            checks.append((int(m.group(2), 16) // 8, int(m.group(1), 16)))
            continue
        m = TYPE8Z_RE.match(text)
        if m:
            checks.append((int(m.group(2), 16) // 8, int(m.group(1), 16)))
            continue
        m = TYPE8R_RE.match(text)
        if m and bregs.get(m.group(1)) is not None:
            checks.append((int(m.group(2), 16) // 8, bregs[m.group(1)]))
            continue
        m = TEST_RE.match(text)
        if m and isinstance(regs.get(m.group(1)), tuple) \
                and regs[m.group(1)][0] == 'arg':
            tests.add(regs[m.group(1)][1])
            continue
        m = CMPI_RE.match(text)
        if m and isinstance(regs.get(m.group(2)), tuple) \
                and regs[m.group(2)][0] == 'arg':
            cmps.setdefault(regs[m.group(2)][1], []).append(
                s32(int(m.group(1), 16)))
            continue
        m = WRITE16_RE.match(text)
        if m and m.group(3) in ('cx', 'eax', 'edx', 'esi') \
                and isinstance(regs.get(m.group(1)), tuple) \
                and regs[m.group(1)][0] == 'arg':
            writes.append({'arg': regs[m.group(1)][1],
                           'offset': int(m.group(2), 16), 'size': 2})
            continue
        m = WRITE8_RE.match(text)
        if m and isinstance(regs.get(m.group(1) + 'x'), tuple) \
                and regs[m.group(1) + 'x'][0] == 'arg':
            writes.append({'arg': regs[m.group(1) + 'x'][1],
                           'offset': int(m.group(2), 16), 'size': 1})
            continue
        m = JMP_RE.match(text)
        if m:
            tails.append('jmp 0x%08x' % int(m.group(1), 16))
            continue
        m = CALL_RE.match(text)
        if m:
            tails.append('call 0x%08x' % int(m.group(1), 16))
            continue

    # 每个实参的取值范围：test/jl 表示下界 0，cmp 立即数成对给上下界
    ranges = {}
    for arg in sorted(set(list(cmps.keys()) + list(tests))):
        vals = cmps.get(arg) or []
        lo = 0 if arg in tests else (min(vals) if vals else None)
        hi = max(vals) if vals else None
        if arg in tests and vals:
            hi = max(vals)
        ranges[str(arg)] = {'min': lo, 'max': hi}
    # 值钳位：mov $imm,<值寄存器>（TextShadowDist 的 0/12 就是这种）
    return {
        'arg_types': [{'arg': a, 'type': t} for a, t in checks],
        'register_compares': register_cmps,
        'immediate_writes': imm_writes,
        'arg_ranges': ranges,
        'field_writes': writes,
        'layer_table': layer_table,
        'globals': ['0x%x' % g for g in sorted(globals_)],
        'calls_or_jumps': tails,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM) or not os.path.isfile(HANDLERS):
        print('SKIP: 缺原版程序清单或 handler 表')
        return 0

    table = {e['id']: e for e in
             json.load(io.open(HANDLERS, encoding='utf-8'))}
    rows = load_rows(ASM)
    result = {'note': '从 exe handler 静态分析抄出的文本样式命令字段/范围（A 类事实）；'
                      '由 scripts/extract-exe-text-params.py 生成',
              'commands': {}}
    for cid, name in sorted(TARGETS.items()):
        e = table.get(cid)
        if e is None:
            continue
        win = window(rows, e['handler'])
        info = extract(win)
        info.update({'id': cid, 'name': name, 'argc': e['argc'],
                     'handler': '0x%08x' % e['handler'],
                     'reg_index': e.get('reg_index')})
        result['commands'][str(cid)] = info
        print('%3d %-14s argc=%d handler=0x%08x' %
              (cid, name, e['argc'], e['handler']))
        if info['layer_table']:
            print('    文本层表：settings+0x%x，stride %d'
                  % (info['layer_table']['offset'],
                     info['layer_table']['stride']))
        for t in info['arg_types']:
            print('    arg%d 必须是 int(type=%d)' % (t['arg'], t['type']))
        for a, r in sorted(info['arg_ranges'].items(), key=lambda kv: int(kv[0])):
            print('    arg%s 范围 %s..%s' % (a, r['min'], r['max']))
        for w in info['field_writes']:
            print('    arg%d -> 样式对象+0x%x（%d 字节）'
                  % (w['arg'], w['offset'], w['size']))
        for t in info['calls_or_jumps']:
            print('    %s' % t)
        print()

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
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