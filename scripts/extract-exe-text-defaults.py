#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 原版程序里抄出**文本窗口构造函数的默认值**与尺寸上限（A 类事实）。

为什么需要它：上一轮"文本样式字段读写点地图"里发现，样式字段的写点除了命令 handler，
还有一处 0x430EA0 的函数。把它读透之后发现它其实是**文本窗口的构造函数**——它把一整套
样式字段初始化成引擎默认值。我们的实现里这些默认值之前是"照文档抄的"
（12x12、字体号 0），跟 exe 不一致，属于"操作上不一样"的那类偏差。

抄法（不猜，只抄）：

  1. 构造函数靠特征指令定位：窗口里出现 `movb $0x10,0x592(%esi)`（默认字号）与
     `movb $0x1,0x5d4(%esi)`，且函数边界是 `ret $0x8`；
  2. 用一个极简寄存器跟踪把 `mov* $imm,0xOFF(%esi)` / `mov* %reg,0xOFF(%esi)`
     （reg 由 `xor reg,reg`/`mov $imm,reg`/`mov %reg,%reg` 推出）还原成
     `字段 = 值`；写入宽度由助记符后缀或源寄存器宽度决定；
  3. 同一个窗口里的 `cmp $imm,%reg` 收集成"尺寸上限"（构造函数的参数检查）；
  4. 清空例程（含 `mov %ax,0x5e8(%esi)` 的那个函数）里，记录每个字段被重置成什么
     （立即数 / 0 / 从另一个字段搬）。

产物 `ref/exe-text-defaults.json`（只含参数，不含游戏数据）。

用法：
    python scripts/extract-exe-text-defaults.py            # 打印 + 写 json
    python scripts/extract-exe-text-defaults.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-defaults.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
MOVIMM_RE = re.compile(r'^mov(l|w|b)?\s+\$0x([0-9a-f]+),%e([a-z]{2})$')
MOVIMM8_RE = re.compile(r'^mov\s+\$0x([0-9a-f]+),%([a-d])l$')
XOR_RE = re.compile(r'^xor\s+%e([a-z]{2}),%e\1$')
MOVREG_RE = re.compile(r'^mov\s+%e([a-z]{2}),%e([a-z]{2})$')
MOVREG16_RE = re.compile(r'^mov\s+%([a-d]x|si|di),%([a-d]x|si|di)$')
MOVREG8_RE = re.compile(r'^mov\s+%([a-d])l,%([a-d])l$')
WRITE_RE = re.compile(
    r'^mov(l|w|b)?\s+(?:\$0x([0-9a-f]+)|%e([a-z]{2})|%([a-d])l|'
    r'%([a-d]x|si|di|bp|sp)),0x([0-9a-f]+)\(%e([a-z]{2})\)$')
CMPI_RE = re.compile(r'^cmp\s+\$0x([0-9a-f]+),%e([a-z]{2})$')
LOAD_RE = re.compile(
    r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
LOAD16_RE = re.compile(r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),'
                       r'%([a-d]x|si|di|bp|sp)$')

FIELD_NAMES = {
    0x590: 'font_id（word）', 0x592: '默认字号宽', 0x593: 'font_id2（byte）',
    0x594: '默认字号高', 0x595: '（byte，构造时 0）', 0x596: '（byte，构造时 0）',
    0x597: 'shadow_dist', 0x598: '颜色来源指针', 0x59C: '颜色来源指针',
    0x5A0: '颜色来源指针', 0x5A4: 'TextFunction arg2', 0x5A5: 'TextFunction arg1',
    0x5A6: 'TextFunction arg3', 0x5A8: 'space_x', 0x5AA: 'space_y',
    0x5AC: 'fmt_pad_b（行宽下限/初始累计宽度）', 0x5AE: 'fmt_pad_a',
    0x5B0: 'fmt_off_b', 0x5B4: 'fmt_off_a', 0x5D4: '（byte，构造时 1）',
    0x5D6: '（byte，清空时 0）', 0x5D8: '（byte，清空时 0）',
    0x5D9: '（byte，构造时 1）', 0x5DC: '窗口宽（u16）', 0x5DE: '窗口高（u16）',
    0x5E0: 'speed_ms', 0x5E8: '行宽累计', 0x5EA: '（word，清空时 0）',
    0x640: '（dword，清空时 0）',
}


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def find_func(rows, pred):
    """pred(idx, text) -> 命中位置，往回找函数边界（上一处 ret/int3 之后）。"""
    for i, (a, t) in enumerate(rows):
        if pred(i, t):
            j = i
            while j > 0:
                prev = rows[j - 1][1]
                if prev.startswith('ret') or prev.startswith('int3'):
                    break
                j -= 1
            k = i
            while k < len(rows):
                if rows[k][1].startswith('ret'):
                    break
                k += 1
            return j, k
    return None


def reg_width(reg):
    if reg in ('ax', 'cx', 'dx', 'bx', 'si', 'di', 'bp', 'sp'):
        return 2 if reg in ('ax', 'cx', 'dx', 'bx') else 4
    return 4


def parse_window(rows, lo, hi, base):
    regs = {}
    bregs = {}
    writes = []
    cmps = []
    loads = []
    for i in range(lo, hi + 1):
        a, t = rows[i]
        m = MOVIMM_RE.match(t)
        if m:
            regs[m.group(3)] = ('imm', int(m.group(2), 16))
            continue
        m = MOVIMM8_RE.match(t)
        if m:
            bregs[m.group(2)] = ('imm', int(m.group(1), 16))
            regs[m.group(2) + 'x'] = ('imm', int(m.group(1), 16))
            continue
        m = XOR_RE.match(t)
        if m:
            regs[m.group(1)] = ('imm', 0)
            continue
        m = MOVREG_RE.match(t)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            continue
        m = MOVREG16_RE.match(t)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            continue
        m = MOVREG8_RE.match(t)
        if m:
            regs[m.group(2) + 'x'] = regs.get(m.group(1) + 'x')
            continue
        m = LOAD_RE.match(t)
        if m:
            loads.append({'pc': '0x%x' % a, 'instr': t,
                          'field': int(m.group(1), 16)})
            regs[m.group(3)] = ('load', int(m.group(1), 16))
            continue
        m = LOAD16_RE.match(t)
        if m:
            loads.append({'pc': '0x%x' % a, 'instr': t,
                          'field': int(m.group(1), 16)})
            regs[m.group(3)] = ('load', int(m.group(1), 16))
            continue
        m = CMPI_RE.match(t)
        if m:
            cmps.append({'pc': '0x%x' % a, 'imm': int(m.group(1), 16),
                         'instr': t})
            continue
        m = WRITE_RE.match(t)
        if m and m.group(7) == base:
            suffix = m.group(1) or ''
            off = int(m.group(6), 16)
            if m.group(2) is not None:
                val = ('imm', int(m.group(2), 16))
                size = {'b': 1, 'w': 2, 'l': 4}.get(suffix, 4)
            elif m.group(3) is not None:
                val = regs.get(m.group(3))
                size = {'b': 1, 'w': 2, 'l': 4}.get(suffix, 4)
            elif m.group(4) is not None:
                val = regs.get(m.group(4) + 'x')
                size = 1
            else:
                val = regs.get(m.group(5))
                size = 2
            writes.append({'pc': '0x%x' % a, 'instr': t, 'field': off,
                           'size': size, 'value': val})
            continue
    return writes, cmps, loads


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows = load_rows(ASM)

    ctor = find_func(rows, lambda i, t: t == 'movb   $0x10,0x592(%esi)')
    if ctor is None:
        print('SKIP: 没找到文本窗口构造函数（movb $0x10,0x592(%esi)）')
        return 0
    writes, cmps, _ = parse_window(rows, ctor[0], ctor[1], 'si')
    defaults = {}
    for w in writes:
        if w['field'] < 0x580 or w['field'] > 0x650:
            continue
        val = w['value']
        if isinstance(val, tuple) and val[0] == 'imm':
            entry = {'kind': 'imm', 'value': val[1]}
        elif isinstance(val, tuple) and val[0] == 'load':
            entry = {'kind': 'copy_field', 'value': val[1]}
        else:
            continue
        entry.update({'offset': w['field'], 'size': w['size'],
                      'name': FIELD_NAMES.get(w['field'], ''),
                      'pc': w['pc'], 'instr': w['instr']})
        defaults['0x%x' % w['field']] = entry

    clear = find_func(rows, lambda i, t: t == 'mov    %ax,0x5e8(%esi)')
    resets = []
    if clear is not None:
        cw, _, _ = parse_window(rows, clear[0], clear[1], 'si')
        for w in cw:
            if 0x580 <= w['field'] <= 0x650:
                resets.append({'pc': w['pc'], 'instr': w['instr'],
                               'field': w['field'], 'size': w['size'],
                               'value': w['value']})

    result = {
        'note': '从 exe 文本窗口构造函数抄出的默认值/上限/清空语义（A 类事实）；'
                '由 scripts/extract-exe-text-defaults.py 生成',
        'constructor': {
            'start': '0x%x' % rows[ctor[0]][0],
            'end': '0x%x' % rows[ctor[1]][0],
            'defaults': defaults,
            'size_limits': sorted({c['imm'] for c in cmps
                                   if 0x100 <= c['imm'] <= 0x1000}),
        },
        'clear_routine': {
            'start': '0x%x' % rows[clear[0]][0] if clear else None,
            'end': '0x%x' % rows[clear[1]][0] if clear else None,
            'resets': resets,
        },
    }

    print('== 文本窗口构造函数 0x%x..0x%x 的默认值 =='
          % (rows[ctor[0]][0], rows[ctor[1]][0]))
    for key in sorted(defaults, key=lambda k: int(k, 16)):
        e = defaults[key]
        shown = ('0x%x' % e['value']) if e['kind'] == 'imm' else (
            '从 +0x%x 搬' % e['value'])
        print('  +0x%03x (%-2d 字节) = %-12s %s'
              % (e['offset'], e['size'], shown, e['name']))
    print('== 尺寸上限（构造函数里的 cmp）== %s'
          % result['constructor']['size_limits'])
    print('== 清空例程 ==')
    for r in resets:
        v = r['value']
        shown = ('0x%x' % v[1]) if isinstance(v, tuple) and v[0] == 'imm' else (
            ('从 +0x%x 搬' % v[1]) if isinstance(v, tuple) and v[0] == 'load'
            else repr(v))
        print('  +0x%03x (%d 字节) := %s   %s'
              % (r['field'], r['size'], shown, r['instr']))

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