#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""引擎自建对象表事实的门禁（A 类）。

`scripts/extract-exe-engine-objects.py` 把引擎初始化里那串
`Construct(base, elemsize, count, ctor)`（`0x448833`）的实参逐条抄成
`ref/exe-engine-objects.json`。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 表数 14、总占用 5,864,072 B（= 5.59 MiB）；
  3. 图元表三元组（base engine+0x61DD64 / 0x34 / 4096）与
     `ref/exe-prim-hierarchy.json` **两份独立基线一致**；
  4. 三张 0x34/0x51C 结构表的地址**首尾相接**（sprite → prim_alt → prim），
     说明解出的 (elemsize, count) 不是把两个实参看反了；
  5. 7 个容器构造函数解出的 (elemsize, count, 元素构造函数) 都能在
     初始化注册表里找到同一组三元组（两处独立抄写互相对上）；
  6. 每张表的 elemsize*count == bytes，且 count>0。

缺 `work/iroseka_HD.asm` 时 SKIP 并返回 0（原版程序清单不入库）。

用法：python tests/verify_exe_engine_objects.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-engine-objects.py')
REF = os.path.join(FVP, 'ref', 'exe-engine-objects.json')
REF_PRIM = os.path.join(FVP, 'ref', 'exe-prim-hierarchy.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read(path):
    return io.open(path, encoding='utf-8').read()


def off(s):
    """'engine+0x61DD64' → 0x61DD64"""
    return int(s.split('+')[-1], 16)


def main():
    asm = os.path.join(FVP, 'work', 'iroseka_HD.asm')
    if not os.path.isfile(asm):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % asm)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-exe-engine-objects.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    ref = json.loads(read(REF))
    tables = ref.get('tables') or []
    by_name = {t.get('name'): t for t in tables if t.get('name')}

    print('== 2. 表数与总占用 ==')
    check(len(tables) == 14, '共 14 张表（实际 %d）' % len(tables))
    check(ref.get('table_count') == len(tables), 'table_count 与表数一致')
    check(ref.get('total_bytes') == 5864072,
          '总占用 5,864,072 B（实际 %r）' % ref.get('total_bytes'))
    total = sum(t['bytes'] for t in tables)
    check(total == ref.get('total_bytes'), '逐表 bytes 之和 == total_bytes')
    for t in tables:
        check(t['elemsize'] * t['count'] == t['bytes'],
              '%s: elemsize*count == bytes' % (t['base'],))
        check(t['count'] > 0, '%s: count > 0' % (t['base'],))

    print('== 3. 图元表与 exe-prim-hierarchy.json 两份基线一致 ==')
    prim = by_name.get('prim') or {}
    check(prim.get('base') == 'engine+0x61DD64',
          'prim base = engine+0x61DD64（实际 %r）' % prim.get('base'))
    check(prim.get('elemsize') == 0x34,
          'prim elemsize 0x34（实际 %r）' % prim.get('elemsize'))
    check(prim.get('count') == 0x1000,
          'prim count 4096（实际 %r）' % prim.get('count'))
    hier = json.loads(read(REF_PRIM)) if os.path.isfile(REF_PRIM) else {}
    hp = hier.get('prim') or {}
    check(hp.get('base') == prim.get('base') and
          hp.get('stride') == prim.get('elemsize') and
          hp.get('count') == prim.get('count'),
          '与 exe-prim-hierarchy.json 的 base/stride/count 完全一致')

    print('== 4. 三张结构表首尾相接（防止 elemsize/count 看反）==')
    order = ['sprite', 'prim_alt', 'prim']
    for a, b in zip(order, order[1:]):
        ta, tb = by_name.get(a) or {}, by_name.get(b) or {}
        if not ta or not tb:
            check(False, '缺表 %s 或 %s' % (a, b))
            continue
        end_a = off(ta['base']) + ta['bytes']
        check(end_a == off(tb['base']),
              '%s 结束于 engine+0x%X == %s 的基址（实际 0x%X vs 0x%X）'
              % (a, end_a, b, end_a, off(tb['base'])))

    print('== 5. 容器构造函数与初始化注册表互相对上 ==')
    ctor = ref.get('container_ctors') or []
    check(len(ctor) == 7, '容器构造函数 7 处（实际 %d）' % len(ctor))
    matched = 0
    for c in ctor:
        hit = [t for t in tables
               if t['elemsize'] == c['elemsize'] and t['count'] == c['count']
               and int(t['ctor'], 16) == int(c['ector'], 16)]
        if hit:
            matched += 1
        check(bool(hit), '%s 的 (size,count,ctor) 能在注册表里找到（%d 张）'
                         % (c['pc'], len(hit)))
    check(matched == len(ctor), '7/7 全部对上（实际 %d）' % matched)
    check((ref.get('container_ctor_matches') or {}).get('matched') == matched,
          'container_ctor_matches.matched 与实际匹配数一致')
    for c in ctor:
        check(c.get('vtable') and c.get('base_expression') == 'this+0x4',
              '%s: 表内联在容器 this+4，vtable=%s'
              % (c['pc'], c.get('vtable')))

    print('== 6. C 侧交叉核对 ==')
    gfxh = read(os.path.join(FVP, 'src', 'gfx.h'))
    check('GFX_PRIM_MAX 4096' in gfxh,
          'gfx.h 的图元表容量 4096 与 exe 的 prim count 一致')
    gfxc = read(os.path.join(FVP, 'src', 'gfx.c'))
    check('calloc(GFX_PRIM_MAX, sizeof(GfxPrim))' in gfxc,
          'gfx.c 以 GFX_PRIM_MAX 项分配图元表')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 引擎 14 张自建对象表的基址/大小/项数/构造函数与 exe 一致，'
          '图元表三元组两份基线互证')
    return 0


if __name__ == '__main__':
    sys.exit(main())