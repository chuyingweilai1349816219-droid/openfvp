#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""extract-exe-vm-ops.py -- 把 exe 的 **VM 指令表 + 变体比较矩阵** 抄成基线。

为什么要这个：2026-11-27 复核 ① 的循环 `0x59264..0x5928C` 时发现，脚本解出来是
`push 常量` / `eq` / `jmpcond` 三件套，而"常量到底是 true 还是 false"直接决定
循环能不能出来。以前所有笔记都按 `ref/fvm_ref.py` 里的名字走（0x08=pushtrue、
0x09=pushfalse），但 exe 的 handler 写的是相反的字节：

    0x444940（op 0x08）: movb $0x0,0x8(%ecx,%eax,8)   ← 类型字节 0 = false/null
    0x444960（op 0x09）: movb $0x1,0x8(%ecx,%eax,8)   ← 类型字节 1 = true

而 `jmpcond`（0x445A30）判的就是这个字节（`cmpb $0x0,(%edx)`），`test`（0x444E40）
也用它当结果。所以 **0x08 是 false、0x09 是 true**，`fvm_ref.py` 与 `src/vm.c` 里
的名字是互换的。

本脚本冻结三件事（全部从 PE 字节读，不依赖反汇编文本）：
1. 指令表：`0x4815E0 + op*4` 的 40 项 handler 地址（由初始化函数的 `movl $imm,槽`
   序列还原），以及唯一的派发点 `0x4454C7`；
2. 三张 7×7 变体比较矩阵（`0x481388` / `0x481450` / `0x481518`）：默认填
   `0x444F50`（恒返回 0），再被逐个覆盖；
3. 比较 helper 的语义（int `sete`、float `fucompp`、字符串调用 0x45D088、常量
   0/1 两个 helper）。

用法：
    python scripts/extract-exe-vm-ops.py            # 打印 + 写 json
    python scripts/extract-exe-vm-ops.py --check     # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-vm-ops.json')

# 派发点：movzbl (code+pc) -> mov 0x4815E0(,%edx,4),%eax -> call *%eax
DISPATCH_VA = 0x4454C7
DISPATCH_BYTES = '8b0495e0154800'
OP_BASE = 0x4815E0
OP_COUNT = 0x28                      # 0x00..0x27，表在 0x4815E0..0x48167F

INIT_START = 0x4463E0                # 指令表/矩阵的初始化函数
INIT_END = 0x446800

# 三张比较矩阵：49 项 × 4 字节 = 0xC4，正好一个 `add $0x1c` / `cmp $0xc4` 的循环节
MATRICES = [
    {'base': 0x481388, 'ops': [0x25], 'name': 'le'},
    {'base': 0x481450, 'ops': [0x24], 'name': 'gt'},
    {'base': 0x481518, 'ops': [0x22, 0x23], 'name': 'eq/neq'},
]
DEFAULT_FILL = 0x444F50               # 恒返回 0
CONST_ONE = 0x444F70                 # 恒返回 1

# handler 头部的字节指纹：用来证明"谁写哪个类型字节"
HANDLER_HEADS = {
    0x08: (0x444940, '8b81200800008d5001899120080000c644c10800c3',
           'pushfalse：整段函数只写类型字节 0'),
    0x09: (0x444960, '8b81200800008d5001899120080000c644c10801c3',
           'pushtrue：整段函数只写类型字节 1'),
    0x0d: (0x445B10, '518b81200800008d5001899120080000c644c10803',
           'pushfloat：写类型字节 3，flds 读 4 字节'),
    0x14: (0x4449F0, '8b91140800008b8120080000',
           'pushtemp：读 +0x814/+0x818 压栈'),
    0x1f: (0x444E40, '568bf1ff8e200800008b8620080000803cc602',
           'test：两个操作数都是 type 2 才做位测'),
    0x07: (0x445A30, 'ff8920080000', 'jmpcond：类型字节为 0 才跳'),
}

# 比较 helper 的语义（地址 -> 结论 + 证据字节）
HELPERS = [
    {'va': 0x444F50, 'what': '恒返回 0（矩阵默认值）', 'bytes': '8b442408 56 50 8bf1'.replace(' ', '')},
    {'va': 0x444F70, 'what': '恒返回 1（eq 的"相等"分支）', 'bytes': '8b442408 56 50 8bf1'.replace(' ', '')},
    {'va': 0x444FA0, 'what': 'int 相等：cmp 载荷 + sete', 'bytes': '8b4424048b4804'},
    {'va': 0x444FC0, 'what': 'float 相等：flds/fucompp + test $0x44,%ah', 'bytes': '8b442404d94004'},
    {'va': 0x444FF0, 'what': '字符串相等：call *0x45D088', 'bytes': '8b41048b4c2408'},
    {'va': 0x4450A0, 'what': 'int 大于：cmp 载荷 + setg', 'bytes': '8b4424048b4804'},
]

TYPE_NAMES = {
    0: 'null / false',
    1: 'bool true',
    2: 'int',
    3: 'float',
    4: '数据段字符串（绝对偏移）',
    5: '动态字符串表槽号',
    6: '0x6A4DF0 表的索引',
}


def sections(buf):
    """PE 段表：[(va, vsize, raw_off, raw_size, name)]"""
    e = struct.unpack_from('<I', buf, 0x3C)[0]
    nsec = struct.unpack_from('<H', buf, e + 6)[0]
    opt = e + 24
    opt_size = struct.unpack_from('<H', buf, e + 20)[0]
    out = []
    for i in range(nsec):
        off = opt + opt_size + i * 40
        name = buf[off:off + 8].rstrip(b'\x00').decode('latin-1')
        vsize, va, raw_size, raw_off = struct.unpack_from('<IIII', buf, off + 8)
        out.append((va, vsize, raw_off, raw_size, name))
    return out


class PE(object):
    def __init__(self, path=EXE):
        self.buf = io.open(path, 'rb').read()
        self.secs = sections(self.buf)
        e = struct.unpack_from('<I', self.buf, 0x3C)[0]
        self.imagebase = struct.unpack_from('<I', self.buf, e + 24 + 28)[0]

    def off(self, va):
        # 段表用的是 RVA；反汇编/文档里的地址是 VA（含 ImageBase，0x400000）
        rva = va - self.imagebase if va >= self.imagebase else va
        for sva, vsize, raw_off, raw_size, _name in self.secs:
            if sva <= rva < sva + max(vsize, raw_size):
                return raw_off + (rva - sva)
        raise KeyError('va 0x%X（rva 0x%X）不在任何段里' % (va, rva))

    def at(self, va, n):
        o = self.off(va)
        return self.buf[o:o + n]

    def u32(self, va):
        return struct.unpack('<I', self.at(va, 4))[0]

    def hexs(self, va, n):
        return self.at(va, n).hex()


def parse_init(pe):
    """顺序扫初始化函数里的三种写法：
         c7 05 <slot32> <imm32>   movl $imm,slot
         b8 <imm32>               mov $imm,%eax
         a3 <slot32>              mov %eax,slot
    遇到不认识的字节就报错（不猜）。"""
    va = INIT_START
    eax = None
    writes = {}
    order = []
    while va < INIT_END:
        b = pe.at(va, 1)
        if b == b'\xc7':
            op2 = pe.at(va + 1, 1)
            if op2 != b'\x05':
                raise ValueError('0x%X: c7 %s 不是 movl $imm,abs' % (va, op2.hex()))
            slot = pe.u32(va + 2)
            imm = pe.u32(va + 6)
            writes[slot] = imm
            order.append({'va': '0x%06x' % va, 'slot': '0x%06x' % slot,
                          'value': '0x%06x' % imm})
            va += 10
        elif b == b'\xb8':
            eax = pe.u32(va + 1)
            va += 5
        elif b == b'\xa3':
            slot = pe.u32(va + 1)
            if eax is None:
                raise ValueError('0x%X: mov %%eax,abs 之前没有 mov $imm,%%eax' % va)
            writes[slot] = eax
            order.append({'va': '0x%06x' % va, 'slot': '0x%06x' % slot,
                          'value': '0x%06x' % eax, 'via': '%eax'})
            va += 5
        elif b == b'\xc3':
            break
        elif b in (b'\x33', b'\xb9', b'\xeb', b'\x8d', b'\x89', b'\x83', b'\x3d',
                   b'\x0f', b'\xcc', b'\x90'):
            # xor eax,eax / mov $imm,%ecx / jmp / lea / mov %ecx,disp(%eax) /
            # add / cmp imm / jl / int3 / nop：长度不一时逐个跳过
            va += skip_insn(pe, va)
        else:
            raise ValueError('0x%X: 未处理的字节 %s' % (va, b.hex()))
    return writes, order


def skip_insn(pe, va):
    """初始化函数里允许出现的"非写入"指令：只认长度，不认语义。"""
    head = pe.hexs(va, 8)
    for pat, size in (
            ('33c0', 2),          # xor %eax,%eax
            ('b9', 5),            # mov $imm32,%ecx
            ('eb', 2),            # jmp rel8
            ('8da42400000000', 7),  # lea 0x0(%esp),%esp
            ('8988', 6),          # mov %ecx,disp32(%eax)
            ('83c0', 3),          # add $imm8,%eax
            ('3d', 5),            # cmp $imm32,%eax
            ('0f8c', 6),          # jl rel32
            ('cc', 1),            # int3
            ('90', 1),            # nop
    ):
        if head.startswith(pat):
            return size
    raise ValueError('0x%X: 未处理的指令头 %s' % (va, head))


def build():
    pe = PE(EXE)
    disp = pe.hexs(DISPATCH_VA, 7)
    if disp != DISPATCH_BYTES:
        raise ValueError('派发点 0x%X 的字节变了: %s' % (DISPATCH_VA, disp))

    writes, order = parse_init(pe)

    ops = []
    for op in range(OP_COUNT):
        slot = OP_BASE + op * 4
        if slot not in writes:
            raise ValueError('指令 0x%02X 的 handler 槽 0x%X 没被初始化' % (op, slot))
        ops.append({'op': op, 'handler': '0x%06x' % writes[slot]})

    mats = []
    for spec in MATRICES:
        entries = []
        for idx in range(49):
            slot = spec['base'] + idx * 4
            entries.append('0x%06x' % writes.get(slot, DEFAULT_FILL))
        overrides = [{'idx': i, 'pair': [i // 7, i % 7], 'fn': entries[i]}
                     for i in range(49) if entries[i] != '0x%06x' % DEFAULT_FILL]
        mats.append({
            'name': spec['name'],
            'base': '0x%06x' % spec['base'],
            'ops': ['0x%02x' % o for o in spec['ops']],
            'index_rule': 'idx = 7*type(left) + type(right)',
            'default': '0x%06x' % DEFAULT_FILL,
            'entries': entries,
            'overrides': overrides,
            'override_count': len(overrides),
        })

    heads = []
    for op in sorted(HANDLER_HEADS):
        va, sig, what = HANDLER_HEADS[op]
        got = pe.hexs(va, len(sig) // 2)
        if got != sig:
            raise ValueError('op 0x%02X 的 handler 0x%X 指纹不符: %s != %s'
                             % (op, va, got, sig))
        heads.append({'op': op, 'handler': '0x%06x' % va,
                      'bytes': got, 'what': what})

    helpers = []
    for h in HELPERS:
        got = pe.hexs(h['va'], len(h['bytes']) // 2)
        if got != h['bytes']:
            raise ValueError('helper 0x%X 指纹不符: %s' % (h['va'], got))
        helpers.append({'va': '0x%06x' % h['va'], 'what': h['what'],
                        'bytes': got})

    return {
        'schema': 'fvp-exe-vm-ops/1',
        'source': {
            'exe': 'iroseka_HD.exe',
            'exe_size': os.path.getsize(EXE),
            'method': 'PE 段表 + 初始化函数的 movl $imm,槽 序列（原样读字节，不猜）',
            'generated_by': 'scripts/extract-exe-vm-ops.py',
        },
        'dispatch': {
            'site': '0x%06x' % DISPATCH_VA,
            'bytes': disp,
            'what': 'op = code[pc++]; handler = table[0x4815E0 + op*4]',
            'table_base': '0x%06x' % OP_BASE,
            'table_count': OP_COUNT,
            'bounded': '0x4815E0 + 0x28*4 = 0x481680，正好接在 0x481518 的 eq 矩阵后',
        },
        'opcodes': ops,
        'type_names': {str(k): v for k, v in sorted(TYPE_NAMES.items())},
        'push_constants': {
            '0x08': {'handler': '0x444940', 'type_byte': 0,
                     'means': 'false / null', 'name': 'pushfalse'},
            '0x09': {'handler': '0x444960', 'type_byte': 1,
                     'means': 'true', 'name': 'pushtrue'},
        },
        'handler_heads': heads,
        'matrices': mats,
        'helpers': helpers,
        'semantics': {
            'variant_slot': '线程栈槽 8 字节：+0 = 类型字节，+4 = 载荷（int/float/偏移）',
            'bool_encoding': '类型字节本身就是布尔值：0 = false（同时是 null）、'
                             '1 = true；载荷无意义。所以 `pushfalse` 只写类型字节 0，'
                             '`pushtrue` 只写类型字节 1。',
            'truthiness': 'jmpcond（0x445A30）只看类型字节是否为 0；因此 type 2 的'
                          ' `int 0` 也被判为**真**——条件必须是 bool 值。',
            'test': '0x444E40：两个操作数都是 type 2 时算 `(1 << b) & a`，否则结果 false。',
            'pushtemp': '0x4449F0：把 +0x814/+0x818（上一次 syscall 的返回槽）压栈，'
                        '然后把 +0x814 清 0 —— 所以"这次 syscall 没写返回"= 类型 0。',
            'eq_matrix_read': '0x444F10（op 0x22 eq）取 f = 矩阵[7*typeA+typeB]，'
                              '结果 = (f != 0)；0x445020（op 0x23 neq）同一矩阵，'
                              '结果 = (f == 0)。',
            'eq_bool_pairs': '矩阵里 type 1 参与的 (1,1)(1,2)(1,3)(1,4)(1,6)(2,1)(3,1) '
                             '全被改写成"恒返回 1"；也就是说 **任何与 true 比较（除 null/'
                             '动态串外）都相等**。这条不是笔误，是原版行为。',
        },
        'note': '由 scripts/extract-exe-vm-ops.py 从 iroseka_HD.exe 直接读字节生成；'
                'hexdump 类证据与 docs/vm-commands.md 的叙述对应。',
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args(argv)

    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在' % EXE)
        return 0

    data = build()
    text = json.dumps(data, ensure_ascii=False, indent=1) + '\n'

    if args.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与 exe 现状不一致' % OUT_JSON)
            return 1
        print('PASS: exe-vm-ops 与 exe 字节一致（%d 条指令、%d 张矩阵）'
              % (len(data['opcodes']), len(data['matrices'])))
        return 0

    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('指令表（0x4815E0 起，%d 条）：' % len(data['opcodes']))
    for row in data['opcodes']:
        print('  0x%02x -> %s' % (row['op'], row['handler']))
    for m in data['matrices']:
        print('矩阵 %-6s @ %s 默认 %s，覆盖 %d 项：%s'
              % (m['name'], m['base'], m['default'], m['override_count'],
                 ', '.join('%d,%d' % tuple(o['pair']) for o in m['overrides'])))
    print('WROTE %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())