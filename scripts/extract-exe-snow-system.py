#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""雪系统记录（`engine+0x684378 + idx*0x5048`）的字段访问图（A 类）。

用途：`0x42AA10`（type-7 / snow 的绘制）里所有 `0x5xxx(%edi)` 都是对一条雪系统记录的
访问。这里把这些访问**全量**抄出来，按"读/写 + 数据类型"分类，作为实现雪的最后一块
依据（本轮只做字段图，不实现粒子数学）。

用法：
    python scripts/extract-exe-snow-system.py            # 打印 + 写 json
    python scripts/extract-exe-snow-system.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-snow-system.json')

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
FIELD = re.compile(r'(0x5[0-9a-f]{3})\(%edi\)')
LO, HI = 0x42AA10, 0x42AE40


def classify(ins):
    """按助记符判断读/写与位宽。"""
    m = re.match(r'^([a-z0-9]+)', ins)
    mn = m.group(1) if m else ''
    if mn.startswith('fildl'):
        return 'read', 'int32 -> float'
    if mn.startswith('flds'):
        return 'read', 'float32'
    if mn.startswith('fistp') or mn.startswith('fstps'):
        return 'write', 'float32'
    if mn.startswith('cmpb'):
        return 'read', 'u8'
    if mn.startswith('cmpw'):
        return 'read', 'u16'
    if mn.startswith('cmpl'):
        return 'read', 'u32'
    if mn.startswith('movzbl'):
        return 'read', 'u8'
    if mn.startswith('movzwl'):
        return 'read', 'u16'
    if mn.startswith('movsbl'):
        return 'read', 'i8'
    if mn.startswith('movswl'):
        return 'read', 'i16'
    if mn.startswith('movb'):
        # movb $x,off 是写；movb off,%reg 是读
        return ('write' if re.search(r',0x5[0-9a-f]{3}\(%edi\)', ins) else 'read',
                'u8')
    if mn.startswith('mov') or mn.startswith('lea'):
        return ('write' if re.search(r'%e[a-z]{2},0x5[0-9a-f]{3}\(%edi\)', ins)
                else 'read', 'u32')
    return 'other', ''


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺反汇编清单 %s' % ASM)
        return 0

    accesses = []
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if not m:
            continue
        a = int(m.group(1), 16)
        if not (LO <= a < HI):
            continue
        ins = m.group(3).split('#')[0].strip()
        fm = FIELD.search(ins)
        if not fm:
            continue
        kind, width = classify(ins)
        accesses.append({'pc': '0x%06x' % a, 'offset': fm.group(1),
                         'kind': kind, 'width': width, 'instr': ins})

    fields = {}
    for x in accesses:
        f = fields.setdefault(x['offset'], {'read': 0, 'write': 0,
                                            'widths': set()})
        f[x['kind']] = f.get(x['kind'], 0) + 1
        if x['width']:
            f['widths'].add(x['width'])
    field_list = []
    for off in sorted(fields, key=lambda o: int(o, 16)):
        f = fields[off]
        field_list.append({'offset': off, 'reads': f['read'],
                           'writes': f['write'],
                           'widths': sorted(f['widths'] or [])})

    # 记录里用到的 D3D 设备方法（vtable 偏移）
    vt = []
    for line in io.open(ASM, encoding='utf-8', errors='replace'):
        m = LINE.match(line.rstrip('\n'))
        if not m:
            continue
        a = int(m.group(1), 16)
        if not (LO <= a < HI):
            continue
        ins = m.group(3).split('#')[0].strip()
        mm = re.match(r'^mov\s+0x([0-9a-f]+)\(%ecx\),%edx$', ins)
        if mm:
            vt.append({'pc': '0x%06x' % a, 'vtable_offset': '0x' + mm.group(1)})

    result = {
        'note': '雪系统记录（engine+0x684378 + idx*0x5048）在绘制函数 0x42AA10 里的'
                '字段访问图（A 类）；由 scripts/extract-exe-snow-system.py 生成',
        'draw_fn': '0x42aa10',
        'record': {'base': 'engine+0x684378', 'stride': '0x5048',
                   'enabled_flag': 'engine+0x689378 + idx*0x5048',
                   'selector': 'prim+0x2E（有符号）'},
        'field_accesses': accesses,
        'fields': field_list,
        'device_calls': vt,
        'hints': [
            {'offset': '0x500c', 'hint': '精灵下标：`imul $0x51c` 后在 0x1014C0/0x1018C8 '
                                        '（sprite 表）里取条目'},
            {'offset': '0x5010', 'hint': '粒子条数（`mov ...,%eax` 后 `dec %eax` 当上界）'},
            {'offset': '0x5014', 'hint': '另一维计数（同样 dec 后当上界）'},
            {'offset': '0x501c/0x5020/0x5024', 'hint': '三个 int→float 的参数'
                                                     '（`fildl` 后参与浮点运算）'},
            {'offset': '0x5028', 'hint': '整型参数（读入后进浮点栈）'},
            {'offset': '0x503c/0x5040/0x5044', 'hint': '三个整型参数，两两做差'
                                                     '（0x5044-0x503c、0x5040-0x5044）'},
        ],
        'unknown': ['字段之间的运算关系（粒子位置/速度/寿命的公式）',
                    '设备调用序列里各 render state 的常量含义'],
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 雪系统记录字段（%d 个偏移，%d 次访问）=='
          % (len(field_list), len(accesses)))
    for f in field_list:
        print('  %-8s read=%-3d write=%-3d widths=%s'
              % (f['offset'], f['reads'], f['writes'], ','.join(f['widths'])))
    print('== 设备方法调用（vtable 偏移，%d 处）==' % len(vt))
    print('  ' + ' '.join(sorted(set(x['vtable_offset'] for x in vt))[:12]))
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