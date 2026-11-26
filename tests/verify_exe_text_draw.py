#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本面板落点/尺寸事实的门禁（A 类）。

`scripts/extract-exe-text-draw.py` 把 `0x42AE40`（type-4/5 共用的贴图绘制）的目标
矩形算法抄成 `ref/exe-text-draw.json`。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 尺寸规则：WH 写过 → prim w/h；否则 → surface `0x510/0x512`（面板尺寸）；
  3. 落点规则：OP 写过 → prim + OP + 父链累积；否则 → prim + surface `0x518/0x51a`
     + 父链累积；`0x514/0x516` 是源裁剪原点（不是落点）；
  4. `PrimSetText(86)` 清掉 WH 位（`0x43BDE1`）⇒ 文本面板一定走"面板尺寸"这一支；
  5. C 侧交叉核对：`gfx.c` 的 type-5 路径用 `prim_abs_xy()` 取绝对坐标、
     `draw_text_window()` 画面板；`TextBuff` 把尺寸存进 `panel_w/panel_h`。

缺 `work/iroseka_HD.asm` 时 SKIP 并返回 0。

用法：python tests/verify_exe_text_draw.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-text-draw.py')
REF = os.path.join(FVP, 'ref', 'exe-text-draw.json')

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
          'extract-exe-text-draw.py --check 退出码 0（实际 %d）' % proc.returncode)

    ref = json.loads(read(REF))
    size = ref.get('size') or {}
    pos = ref.get('position') or {}

    print('== 2. 尺寸规则 ==')
    check('prim+0x18' in (size.get('if_wh_written') or ''),
          'WH 写过 → prim+0x18/0x1a（实际 %r）' % (size.get('if_wh_written'),))
    check('0x510' in (size.get('else') or ''),
          'WH 没写过 → surface 0x510/0x512（实际 %r）' % (size.get('else'),))
    check('0x43BDE1' in (size.get('why_text_uses_panel') or ''),
          '写明 PrimSetText 的 0x43BDE1 清 WH 位 ⇒ 文本走面板尺寸')

    print('== 3. 落点规则 ==')
    check('0x20' in (pos.get('if_op_written') or '') and
          '0x518' in (pos.get('else') or ''),
          'OP 写过用 prim OP，否则用 surface 0x518/0x51a')
    check('裁剪' in (pos.get('source_crop') or ''),
          '0x514/0x516 标注为源裁剪原点（不是落点）')

    print('== 4. 关键指令都在 ==')
    pcs = {c['pc']: c['instr'] for c in ref.get('instructions') or []}
    for pc in ('0x438c54', '0x42aec8', '0x42af08', '0x42b41f', '0x42b42d',
               '0x42b43b', '0x42b445', '0x42b48b', '0x42b492', '0x42b499',
               '0x43bde1'):
        check(pc in pcs, '指令 %s 有记录' % pc)
    check('add    0xc(%ebp),%edx' == pcs.get('0x42b42d'),
          '0x42b42d 把父链累积的 y 加进落点')

    print('== 5. 结论与 C 侧交叉核对 ==')
    concl = ref.get('conclusion') or ''
    check('1:1' in concl and 'prim' in concl,
          '结论写明"面板 1:1 画在 prim 的 (x,y) 上"')
    gfx = read(os.path.join(FVP, 'src', 'gfx.c'))
    check('prim_abs_xy(g, pid, &origin[0], &origin[1]);' in gfx,
          'gfx.c 的 type-5 路径用 prim_abs_xy() 取绝对坐标')
    check('draw_text_window(g, p->tw, origin, p->alpha);' in gfx,
          'gfx.c 用 draw_text_window() 在 (origin) 上画面板')
    check('g->tw[layer].panel_w = w;' in gfx and 'g->tw[layer].panel_h = h;' in gfx,
          'TextBuff 把面板尺寸存进 panel_w/panel_h（= 绘制尺寸）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 文本面板落点/尺寸规则与 exe 一致（1:1 画在 prim 上、尺寸用面板）')
    return 0


if __name__ == '__main__':
    sys.exit(main())