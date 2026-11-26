#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""图元层级/派发事实的门禁（A 类）。

`scripts/extract-exe-prim-hierarchy.py` 把 `0x438AB0`（图元派发）与 `0x438B0F`
（组迭代）里的关键指令逐条抄成 `ref/exe-prim-hierarchy.json`。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 图元结构：base engine+0x61DD64、步长 0x34、4096 项、id 1..0xFFF；
  3. 派发：type 跳表 7 类、无父节点才派发、父链累积用 `add`（x/y 分别加组坐标）、
     文本图元层号字段 0x32、窗口表 engine+0x98、内嵌 surface 偏移 +4；
  4. C 侧交叉核对：`prim_abs_xy()` 实现父链求和、`wh_set` 对应 +0x06 bit0、
     syscall 的 PrimSetText 绑定 tw（GFX_F_TW）。

缺 `work/iroseka_HD.asm` 时 SKIP 并返回 0（原版程序清单不入库）。

用法：python tests/verify_exe_prim_hierarchy.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-prim-hierarchy.py')
REF = os.path.join(FVP, 'ref', 'exe-prim-hierarchy.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read(path):
    return io.open(path, encoding='utf-8').read()


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
          'extract-exe-prim-hierarchy.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    ref = json.loads(read(REF))
    prim = ref.get('prim') or {}
    disp = ref.get('dispatch') or {}
    text = ref.get('text_prim') or {}

    print('== 2. 图元结构 ==')
    check(prim.get('base') == 'engine+0x61DD64',
          'base = engine+0x61DD64（实际 %r）' % prim.get('base'))
    check(prim.get('stride') == 0x34, '步长 0x34（实际 %r）' % prim.get('stride'))
    check(prim.get('count') == 0x1000,
          '共 4096 项（实际 %r）' % prim.get('count'))
    check(prim.get('id_range') == [1, 0xFFF],
          'id 1..0xFFF（实际 %r）' % (prim.get('id_range'),))
    fields = prim.get('fields') or {}
    for off in ('0x04', '0x0c', '0x10', '0x14', '0x16', '0x18', '0x1a',
                '0x2a', '0x32'):
        check(off in fields, '字段 %s 有记录' % off)

    print('== 3. 派发与父链累积 ==')
    check(disp.get('func') == '0x438ab0', '派发函数 0x438ab0')
    check(disp.get('type_count') == 7, 'type 跳表 7 类')
    steps = {s.get('pc'): s.get('what') or '' for s in disp.get('steps') or []}
    check('0x438ac2' in steps, '父链上溯步骤 0x438ac2 有记录')
    check('0x438afa' in steps, '无父才按 type 派发 0x438afa 有记录')
    check('0x438b22' in steps and '0x438b34' not in steps,
          '组迭代把组坐标加到子调用参数（0x438b22）')
    check('父链' in (disp.get('invariant') or ''),
          '不变式写明"屏幕坐标 = 父链上 x/y 之和"')
    check(text.get('layer_field') == 0x32, '文本层号字段 = prim+0x32')
    check(text.get('surface_offset_in_window') == 4,
          '窗口内嵌 surface 偏移 = +4')
    check('0x98' in (text.get('window_table') or ''),
          '窗口表 = engine+0x98')

    print('== 4. C 侧交叉核对 ==')
    gfx = read(os.path.join(FVP, 'src', 'gfx.c'))
    sysc = read(os.path.join(FVP, 'src', 'syscall.c'))
    check('prim_abs_xy' in gfx and 'p = g->prim[p].parent' in gfx,
          'gfx.c 的 prim_abs_xy() 沿父链求和')
    check('wh_set' in gfx, 'gfx.c 有 wh_set（对应 prim+0x06 bit0）')
    check('GFX_F_TW' in sysc and 'case 86' in sysc,
          'syscall 的 PrimSetText(86) 绑定文本层（GFX_F_TW）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 图元结构/派发/父链累积与 exe 一致（正文文本图元的位置来源有出处）')
    return 0


if __name__ == '__main__':
    sys.exit(main())