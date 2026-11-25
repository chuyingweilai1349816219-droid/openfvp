#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把文本样式字段的**读写点**从 exe 原版程序里抄成一张地图（A 类事实）。

上一轮（`ref/exe-text-params.json`）解出了"样式命令把值写进哪个字段、范围多少"。
但"渲染侧怎么用这些字段"还没答：本轮扫出所有**读点/写点/读改写点**，冻结成
`ref/exe-text-fields.json`，回答三个问题：

  1. 谁写这些字段？—— 应该只有样式命令的 handler（0x43CBB0..0x43CE70）
     和一处结构拷贝例程（0x430F4E..0x430F9B）；
  2. 谁读？—— 逐字段列出精确读点；另外把"被更宽的读覆盖"的情况也列出来
     （例如 `movswl 0x594(%ecx)` 会顺带读到 +0x597），避免"没有精确读点"被误读成
     "这个字段没人用"；
  3. 读改写的算术长什么样？—— 例如 +0x5E8 在文本方法里被 `add %ax,0x5e8(%ecx)`
     **累加**（0x4247C4 / 0x4247DA），在换行判断里被 `movswl` 读出后跟 +0x5DC 比较
     （0x433006）—— 这就是"累计行宽"的证据。

分类规则（只看指令文本，不猜）：
  * 写：`mov* %reg,0xOFF(...)` / `mov* $imm,0xOFF(...)`
  * 读改写：`add|sub|and|or|xor|inc|dec ... 0xOFF(...)`
  * 读：`mov* 0xOFF(...),%reg`（宽度由助记符推：movzbl/movsbl=1、movzwl/movswl=2、其余=4）

产物 `ref/exe-text-fields.json`（只含地址与指令文本，不含游戏数据）。

用法：
    python scripts/extract-exe-text-fields.py            # 打印 + 写 json
    python scripts/extract-exe-text-fields.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-text-fields.json')

# 文本样式对象里的字段（相对样式对象基址）
FIELDS = [0x5A4, 0x5A5, 0x5A6, 0x5A8, 0x5AA, 0x5AC, 0x5AE, 0x5B0, 0x5B4,
          0x5D7, 0x597, 0x5E8]

FIELD_NAMES = {
    0x5A4: 'TextFunction arg2（0..2）',
    0x5A5: 'TextFunction arg1（0..1）',
    0x5A6: 'TextFunction arg3（0..2）',
    0x5A8: 'TextSpace/TextFormat arg1（-32..32）',
    0x5AA: 'TextSpace/TextFormat arg2（-32..32）',
    0x5AC: 'TextFormat arg4（0..64）',
    0x5AE: 'TextFormat arg3（0..64）',
    0x5B0: 'TextFormat arg6（-16..16）',
    0x5B4: 'TextFormat arg5（-16..16）',
    0x5D7: 'TextSkip（0..3）',
    0x597: 'TextShadowDist（0..12，经 0x4248B0 写入）',
    0x5E8: '行宽累计（TextFormat arg4 抬下限）',
}

# 这些命令 handler 的地址窗口（写点应该都落在这里或结构拷贝例程里）
HANDLER_WINDOWS = [(0x43CBB0, 0x43CE70), (0x43DA70, 0x43DAE4)]
COPY_WINDOWS = [(0x430E00, 0x431100)]

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
MEM_RE = re.compile(r'0x([0-9a-f]+)\(%e([a-z]{2})\)')
MNEMO_RE = re.compile(r'^([a-z0-9]+)')

WRITE_RE = re.compile(r'^mov[a-z]*\s+(\$0x[0-9a-f]+|%[a-z0-9]+),0x([0-9a-f]+)'
                      r'\(%e[a-z]{2}\)$')
RMW_RE = re.compile(r'^(add|sub|and|or|xor|inc|dec)[a-z]*\s+.*0x([0-9a-f]+)'
                    r'\(%e[a-z]{2}\)$')
READ_RE = re.compile(r'^(mov[a-z]*)\s+0x([0-9a-f]+)\(%e[a-z]{2}\),%[a-z0-9]+$')


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def op_size(mnemo):
    if mnemo in ('movzbl', 'movsbl'):
        return 1
    if mnemo in ('movzwl', 'movswl', 'movw'):
        return 2
    return 4


def func_start(rows, addr):
    i = 0
    for k, (a, _) in enumerate(rows):
        if a == addr:
            i = k
            break
    j = i
    while j > 0:
        t = rows[j - 1][1]
        if t.startswith('ret') or t.startswith('int3'):
            break
        j -= 1
    return rows[j][0]


def in_windows(addr, windows):
    return any(lo <= addr <= hi for lo, hi in windows)


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows = load_rows(ASM)
    result = {'note': '文本样式字段的读/写/读改写点（A 类事实）；'
                      '由 scripts/extract-exe-text-fields.py 从 exe 静态分析提取',
              'fields': {}}
    for f in FIELDS:
        result['fields']['0x%x' % f] = {'name': FIELD_NAMES[f],
                                         'writes': [], 'reads': [],
                                         'rmw': [], 'covering_reads': []}

    for a, t in rows:
        for f in FIELDS:
            key = '0x%x' % f
            ent = result['fields'][key]
            mw = WRITE_RE.match(t)
            if mw and int(mw.group(2), 16) == f:
                ent['writes'].append({'pc': '0x%x' % a, 'instr': t,
                                      'func': '0x%x' % func_start(rows, a),
                                      'in_handler': in_windows(a, HANDLER_WINDOWS),
                                      'in_copy': in_windows(a, COPY_WINDOWS)})
                continue
            mr = RMW_RE.match(t)
            if mr and int(mr.group(2), 16) == f:
                ent['rmw'].append({'pc': '0x%x' % a, 'instr': t,
                                   'func': '0x%x' % func_start(rows, a)})
                continue
            mrd = READ_RE.match(t)
            if mrd:
                off = int(mrd.group(2), 16)
                size = op_size(mrd.group(1))
                if off == f:
                    ent['reads'].append({'pc': '0x%x' % a,
                                         'instr': t,
                                         'func': '0x%x' % func_start(rows, a)})
                elif off < f < off + size:
                    ent['covering_reads'].append(
                        {'pc': '0x%x' % a, 'instr': t,
                         'covers': ['0x%x' % x for x in range(off, off + size)]})

    print('== 文本样式字段的访问地图 ==')
    for f in FIELDS:
        key = '0x%x' % f
        e = result['fields'][key]
        print('+0x%03x %-42s 写 %d / 读 %d / 读改写 %d / 覆盖读 %d'
              % (f, FIELD_NAMES[f], len(e['writes']), len(e['reads']),
                 len(e['rmw']), len(e['covering_reads'])))
        for w in e['writes']:
            tag = 'handler' if w['in_handler'] else (
                'copy' if w['in_copy'] else 'other')
            print('     %s 写 %s  [%s]' % (w['pc'], w['instr'], tag))
        for r in e['reads']:
            print('     %s 读 %s  (func %s)' % (r['pc'], r['instr'], r['func']))
        for r in e['rmw']:
            print('     %s 读改写 %s  (func %s)' % (r['pc'], r['instr'], r['func']))
        for r in e['covering_reads'][:3]:
            print('     %s 覆盖读 %s' % (r['pc'], r['instr']))

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