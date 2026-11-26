#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe + 真实存档抄出**全局变量 Variant 的类型语义**（A 类：ref/exe-variant-types.json）。

每种类型都要有"证据"，没证据的写"未解"，不猜：

  type 0  null / false / 未设     —— 载荷无意义（样本里 118~345 条带随机载荷，最大 7500 万）
  type 1  ？                      —— `save.bin` 那边也标 unknown
  type 2  int                    —— handler 里随处可见的 `cmpb $0x2`
  type 3  ？                      —— 未解
  type 4  数据段字符串的绝对偏移   —— 4 个偏移在 iroseka.hcb 里都解出合法 CP932 串
  type 5  动态字符串表的**槽号**   —— 槽 905 两个 type5 单元是 idx 0/2，而表里非空槽正是 #0/#2
  type 6  **0x6A4DF0 表的索引**   —— handler 0x445CB0：`cmpb $0x6,(%edx,%ecx,8)` 之后
                                     `lea 0x6a4df0(%eax,%ecx,4)`（ecx = payload*5）取记录；
                                     真实存档里 type6 载荷是 0..n-1 的**连续整数**

用法：
    python scripts/extract-exe-variant-types.py            # 打印 + 写 json
    python scripts/extract-exe-variant-types.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-variant-types.json')
SAVE_DIR = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\save'
SAVE_HCB = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'

LINE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

# type 6 的证据：这三条指令必须同时出现（handler 0x445CB0）
TYPE6_EXPECT = [
    (0x445CDB, 'cmpb   $0x6,(%edx,%ecx,8)'),
    (0x445CEF, 'lea    (%ecx,%ecx,4),%ecx'),
    (0x445CF3, 'lea    0x6a4df0(%eax,%ecx,4),%ecx'),
]

TYPES = [
    {'type': 0, 'name': 'null / false / 未设', 'applied': True,
     'evidence': '载荷无意义（样本 118~345 条带随机载荷，最大 7500 万）；'
                 'src/fvp.h 的原版 Variant 注释与 save.bin 加载器都把 type 0 当"未设"'},
    {'type': 1, 'name': 'bool true', 'applied': True,
     'evidence': 'op 0x09 pushtrue（handler 0x444960）整段只写 `c6 44 c1 08 01`，'
                 '即类型字节 1、载荷不动；`jmpcond` 拿非零类型字节当真。'
                 '见 ref/exe-vm-ops.json 的 push_constants'},
    {'type': 2, 'name': 'int', 'applied': True,
     'evidence': 'handler 里满地的 cmpb $0x2（101 处）；载荷 = i32'},
    {'type': 3, 'name': 'float', 'applied': True,
     'evidence': 'op 0x0d pushfloat（handler 0x445B10）写 `c6 44 c1 08 03` 后用 '
                 'flds 读 4 字节、fstps 存进载荷；比较表 (3,3) 走 0x444FC0 的 '
                 'fucompp（浮点相等）'},
    {'type': 4, 'name': '数据段字符串的绝对偏移', 'applied': True,
     'evidence': '143332→"voice/01001530"、518150→"……。"、481990→"None"、'
                 '4199307→"……。"（iroseka.hcb 里都是合法 CP932 NUL 结尾串）'},
    {'type': 5, 'name': '动态字符串表的槽号', 'applied': True,
     'evidence': '槽 905 的两个 type5 载荷是 0 与 2，而非空槽正是 #0/#2（#1 为空）'},
    {'type': 6, 'name': '0x6A4DF0 表的索引', 'applied': False,
     'evidence': 'handler 0x445CB0：cmpb $0x6,(%edx,%ecx,8) + lea 0x6a4df0(%eax,%ecx,4)'
                 '（ecx = payload*5）；真实存档 type6 载荷是 0..n-1 的连续整数；'
                 '要回灌它必须先把第 14 段（0x6A4DF0 表）恢复出来'},
]


def load_rows(path):
    rows = {}
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE.match(line.rstrip('\n'))
            if m:
                rows[int(m.group(1), 16)] = m.group(3).split('#')[0].strip()
    return rows


def save_stats():
    """每份存档的 type6 载荷范围（0..n-1 连续、<256）。"""
    THUMB = 200 * 113 * 4
    POOL_BASE = 0x163A0
    out = []
    if not os.path.isdir(SAVE_DIR):
        return out

    def rd_u16(b, i):
        return struct.unpack_from('<H', b, i)[0]

    def rd_u32(b, i):
        return struct.unpack_from('<I', b, i)[0]

    for name in sorted(os.listdir(SAVE_DIR)):
        if not (name.startswith('s') and name.endswith('.bin')) or \
                name == 'save.bin':
            continue
        buf = io.open(os.path.join(SAVE_DIR, name), 'rb').read()
        pos = 7
        for _ in range(3):
            pos += 2 + rd_u16(buf, pos)
        tail = buf[pos + THUMB:]
        hits = []
        for off in range(POOL_BASE, min(len(tail) - 8, POOL_BASE + 0x4000), 8):
            if rd_u32(tail, off) != 0x20:
                continue
            count = rd_u32(tail, off + 4)
            if count > 8:
                continue
            cur = off + 8
            ok = True
            for _ in range(count):
                n = rd_u16(tail, cur)
                if n < 2 or n > 160 or cur + 2 + n > len(tail) or \
                        tail[cur + 2 + n - 1] != 0:
                    ok = False
                    break
                cur += 2 + n
            if not ok:
                continue
            rest = 2 * (128 - count)
            if cur + rest > len(tail) or any(tail[cur:cur + rest]):
                continue
            hits.append(off)
        if len(hits) != 1:
            continue
        gcount = (hits[0] - POOL_BASE) // 8
        t6 = [struct.unpack_from('<i', tail, i * 8 + 4)[0]
              for i in range(gcount) if tail[i * 8] == 6]
        if t6:
            out.append({'save': name, 'count': len(t6), 'min': min(t6),
                        'max': max(t6), 'distinct': len(set(t6)),
                        'dense': sorted(t6) == list(range(len(t6))),
                        'lt256': all(0 <= v < 256 for v in t6)})
    return out


def string_check():
    """type 4 的四个偏移在 hcb 里解出的字符串（证据）。"""
    out = []
    if not os.path.isfile(SAVE_HCB):
        return out
    hcb = io.open(SAVE_HCB, 'rb').read()
    for off in (143332, 518150, 481990, 4199307):
        if off >= len(hcb):
            continue
        end = hcb.find(b'\x00', off, min(len(hcb), off + 80))
        if end < 0:
            continue
        out.append({'offset': off,
                    'text': hcb[off:end].decode('cp932', 'replace')[:40]})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0
    rows = load_rows(ASM)
    checked = []
    for addr, want in TYPE6_EXPECT:
        got = rows.get(addr)
        if got != want:
            print('FAIL: 0x%x = %r（期望 %r）' % (addr, got, want))
            return 1
        checked.append({'pc': '0x%06x' % addr, 'instr': want})

    result = {
        'note': '全局变量 Variant 的类型语义（A 类，逐条带证据）；由 '
                'scripts/extract-exe-variant-types.py 生成；'
                '读档回灌见 src/platform/win32/main_win32.c 的 shell_load_restart',
        'cell': '8 字节：byte0 = type，i32 载荷在 +4',
        'types': TYPES,
        'type6_evidence': {
            'handler': '0x445cb0',
            'instrs': checked,
            'what': 'cmpb $0x6,(%edx,%ecx,8) 判全局单元是 type6，'
                    'lea 0x6a4df0(%eax,%ecx,4)（ecx = payload*5）取 0x14 字节记录',
            'table': 'engine+0x6A4DF0，256 项 × 0x14（读档第 14 段、写档第 14 段）',
        },
        'type4_strings': string_check(),
        'type6_saves': save_stats(),
        'applied': {'read_restart': [0, 2, 4, 5],
                    'not_applied': [1, 3, 6],
                    'note': 'type6 需要先恢复第 14 段（0x6A4DF0 表）才能解析'},
    }
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 类型表 ==')
    for t in TYPES:
        print('  type %d  %-22s applied=%-5s %s'
              % (t['type'], t['name'], t['applied'], t['evidence'][:46]))
    print('== type4 证据 ==')
    for s in result['type4_strings']:
        print('  %d -> %r' % (s['offset'], s['text']))
    print('== type6 存档证据 ==')
    for s in result['type6_saves']:
        print('  %-9s n=%-3d 0..%d dense=%s <256=%s'
              % (s['save'], s['count'], s['max'], s['dense'], s['lt256']))

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