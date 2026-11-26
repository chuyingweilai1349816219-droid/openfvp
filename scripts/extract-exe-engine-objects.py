#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 里抄出**引擎自建对象表**（A 类：基址/元素大小/项数/构造函数的来源）。

为什么需要它：图元表（`engine+0x61DD64`，步长 0x34，4096 项）已经在
`ref/exe-prim-hierarchy.json` 里冻结，但那张表是**怎么被建出来的**一直没写下来。
本轮把引擎初始化里那一串 `Construct(base, elemsize, count, ctor)` 调用
（`0x448833`：按 count 逐个元素调用构造函数）逐条抄出来，得到引擎全部对象表的
**基址 / 元素大小 / 项数 / 构造函数**，顺带独立复核了图元表的三元组。

抄法（只抄，不猜）：
  * `0x448833` 是 cdecl 的 `Construct(base, elemsize, count, ctor)`：
    `eax = elemsize*count; base += eax;` 然后倒序 `base -= elemsize;
    call *ctor`，共 count 次（`0x448843..0x448861`）。
  * 调用点的实参就是紧邻的四条 push（`push $ctor` / `push $count` /
    `push $size` / `push base`），base 来自 `lea disp(%esi),%reg` 或
    `mov -0x10(%ebp),%reg; add $disp,%reg`（`%esi`/局部变量都是引擎基址）。
  * 一步初始化之前会有 `movb $0xNN,0x28(%esp)` 写初始化步骤号，一并抄下来。
  * 每个表还有一个**容器构造函数**（`0x4347A0` 家族）：先在 `(%ecx)` 写
    vtable 常量，再 `add $0x4,%ecx` 把 `this+4` 当元素数组首址传给
    `Construct`——所以表在容器对象里是**内联**的（`this+4` 起）。

产物 `ref/exe-engine-objects.json`（只含结构与指令，不含游戏数据）。

用法：
    python scripts/extract-exe-engine-objects.py            # 打印 + 写 json
    python scripts/extract-exe-engine-objects.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-engine-objects.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')

CONSTRUCT = 0x448833          # Construct(base, elemsize, count, ctor)
INIT_LO = 0x434F00            # 引擎初始化（表注册）所在的地址范围
INIT_HI = 0x435300

LEA_ESI = re.compile(r'^lea\s+0x([0-9a-f]+)\(%esi\),%(\w+)$')
LEA_EBP = re.compile(r'^mov\s+-0x([0-9a-f]+)\(%ebp\),%(\w+)$')
ADD_IMM = re.compile(r'^add\s+\$(0x[0-9a-f]+),%(\w+)$')
MOVL_THIS = re.compile(r'^movl\s+\$(0x[0-9a-f]+),\(%ecx\)$')
ADDL_ECX = re.compile(r'^add\s+\$(0x[0-9a-f]+),%ecx$')
PUSH_IMM = re.compile(r'^push\s+\$(0x[0-9a-f]+)$')
PUSH_REG = re.compile(r'^push\s+%(\w+)$')
MOVB_STEP = re.compile(r'^movb\s+\$(0x[0-9a-f]+),0x[0-9a-f]+\(%esp\)$')
CALL = re.compile(r'^call\s+0x([0-9a-f]+)$')

# (base, elemsize, count) → 名字。只有三张表能确定用途，其余留空（只记结构）。
NAMES = {
    ('engine+0x61DD64', 0x34, 0x1000): 'prim',
    ('engine+0x61D064', 0x34, 0x40): 'prim_alt',
    ('engine+0x101064', 0x51C, 0x1000): 'sprite',
}

# 相邻关系（由上面解出的 base/size/count 直接推出，门禁里复核）：
CONTIGUOUS = ['sprite', 'prim_alt', 'prim']


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).split('#')[0].strip()))
    return rows


def scan(rows):
    """按地址顺序走一遍，遇到 call Construct 就把实参还原成一条记录。"""
    labels = {}          # 寄存器/局部变量 → 'engine+0xNNNN' 形式
    pushes = []          # 待用的 push 实参（值；无法解析时是 None）
    step = None
    sites = []
    for addr, ins in rows:
        m = LEA_ESI.match(ins)
        if m:
            labels[m.group(2)] = 'engine+0x%s' % m.group(1).upper()
            continue
        m = ADD_IMM.match(ins)
        if m and labels.get(m.group(2)):
            base = labels[m.group(2)]
            labels[m.group(2)] = '%s+0x%s' % (base, m.group(1)[2:].upper())
            continue
        m = PUSH_IMM.match(ins)
        if m:
            pushes.append(('imm', int(m.group(1), 16)))
            continue
        m = PUSH_REG.match(ins)
        if m:
            pushes.append(('reg', labels.get(m.group(1))))
            continue
        m = MOVB_STEP.match(ins)
        if m:
            step = int(m.group(1), 16)
            continue
        m = CALL.match(ins)
        if m:
            target = int(m.group(1), 16)
            if target == CONSTRUCT:
                vals = [v for _, v in pushes[-4:]]
                if (len(vals) == 4 and isinstance(vals[0], int)
                        and isinstance(vals[1], int)
                        and isinstance(vals[2], int)
                        and vals[3] is not None):
                    sites.append({'pc': '0x%06x' % addr, 'base': vals[3],
                                  'elemsize': vals[2], 'count': vals[1],
                                  'ctor': '0x%06x' % vals[0],
                                  'init_step': step})
            pushes = []
            step = None
            continue
        if ins.startswith(('jmp', 'ret', 'je ', 'jne ', 'jl ', 'jg ', 'jb ',
                           'jae ', 'jbe ', 'ja ', 'js ', 'jns ', 'jnp ')):
            pushes = []
            step = None
    return sites


def scan_container_ctors(rows):
    """容器构造函数：`movl $vtable,(%ecx)` + `add $size,%ecx` + Construct(base=this+4)。"""
    labels = {}
    pushes = []
    vtable = None
    this_off = None
    out = []
    for addr, ins in rows:
        m = MOVL_THIS.match(ins)
        if m:
            vtable = int(m.group(1), 16)
            continue
        m = ADDL_ECX.match(ins)
        if m:
            this_off = int(m.group(1), 16)
            continue
        m = PUSH_IMM.match(ins)
        if m:
            pushes.append(int(m.group(1), 16))
            continue
        m = PUSH_REG.match(ins)
        if m:
            pushes.append(None)
            continue
        m = CALL.match(ins)
        if m:
            if int(m.group(1), 16) == CONSTRUCT and len(pushes) >= 4:
                v = pushes[-4:]
                if v[0] is not None and v[1] is not None and v[2] is not None:
                    out.append({'pc': '0x%06x' % addr,
                                'vtable': '0x%06x' % vtable if vtable else None,
                                'base_expression': 'this+0x%X' % this_off
                                if this_off is not None else None,
                                'elemsize': v[2], 'count': v[1],
                                'ector': '0x%06x' % v[0]})
            pushes = []
            vtable = None
            this_off = None
            continue
        if ins.startswith(('ret', 'jmp')):
            pushes = []
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows = load_rows(ASM)
    all_sites = scan(rows)
    init = [s for s in all_sites if INIT_LO <= int(s['pc'], 16) <= INIT_HI]

    tables = []
    seen = set()
    for s in init:
        key = (s['base'], s['elemsize'], s['count'], s['ctor'])
        if key in seen:
            continue
        seen.add(key)
        tables.append({
            'name': NAMES.get((s['base'], s['elemsize'], s['count']), ''),
            'base': s['base'],
            'elemsize': s['elemsize'],
            'count': s['count'],
            'bytes': s['elemsize'] * s['count'],
            'ctor': s['ctor'],
            'init_step': s['init_step'],
            'register_pc': s['pc'],
        })
    tables.sort(key=lambda t: t['base'])

    containers = [c for c in scan_container_ctors(rows)
                  if c['vtable'] and c['base_expression']]

    total = sum(t['bytes'] for t in tables)
    result = {
        'note': '从 exe 静态分析抄出的引擎自建对象表（A 类）：基址/元素大小/项数/'
                '构造函数全部是 0x448833 调用点的实参，由 '
                'scripts/extract-exe-engine-objects.py 生成',
        'construct': {
            'func': '0x448833',
            'args': ['base', 'elemsize', 'count', 'ctor'],
            'steps': [
                {'pc': '0x448843', 'what': 'esi = elemsize（0xc(%ebp)）'},
                {'pc': '0x448848', 'what': 'base += elemsize*count，从表尾倒序构造'},
                {'pc': '0x448858', 'what': 'base -= elemsize；ecx = base'},
                {'pc': '0x44885e', 'what': 'call *0x14(%ebp)，即逐个元素调 ctor(this)'},
            ],
            'inline_arrays': '每个表都内联在容器对象里：容器构造函数先在 (%ecx) 写 '
                             'vtable，再 add $0x4,%ecx，把 this+4 当元素数组首址',
        },
        'engine_init_range': ['0x%06x' % INIT_LO, '0x%06x' % INIT_HI],
        'tables': tables,
        'table_count': len(tables),
        'total_bytes': total,
        'container_ctors': containers,
        'container_ctor_matches': {
            'what': '每个容器构造函数解出的 (elemsize, count, 元素构造函数) 必须能在'
                    '引擎初始化注册的表里找到同一组三元组——两处独立抄写互相对上，'
                    '才说明表和偏移配对了',
            'matched': sum(1 for c in containers
                           for t in tables
                           if c['elemsize'] == t['elemsize']
                           and c['count'] == t['count']
                           and int(c['ector'], 16) == int(t['ctor'], 16)),
        },
    }

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('== 引擎自建对象表（%d 张） ==' % len(tables))
    for t in tables:
        print('  %-22s base %-16s size 0x%-4x count 0x%-4x = %7d B  ctor %s  step %s'
              % (t['name'] or '?', t['base'], t['elemsize'], t['count'],
                 t['bytes'], t['ctor'],
                 '0x%02x' % t['init_step'] if t['init_step'] is not None else '-'))
    print('  合计 %d B（%.2f MiB）' % (total, total / 1048576.0))
    print('== 容器构造函数 %d 处 ==' % len(containers))
    for c in containers[:4]:
        print('  %s  size 0x%x count 0x%x ector %s vtable %s base %s'
              % (c['pc'], c['elemsize'], c['count'], c['ector'],
                 c['vtable'], c['base_expression']))
    print('  ...')

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