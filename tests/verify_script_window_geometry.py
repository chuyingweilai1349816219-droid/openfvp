#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本窗口几何事实的门禁（A 类）。

`scripts/extract-script-window-geometry.py` 从 HCB 脚本里抄出：
设计坐标 1024×640 → 画布 1920×1080 的换算常量（1.875 / 1.6875，来自
`pushfloat 0x3FF00000 / 0x3FD80000`）、窗口 helper 家族的 syscall 号、
四个文本窗口的绑定（prim / 层号 / 设计坐标 → 画布坐标）、以及 TextBuff 的
1024 钳位。本门禁断言：

  1. 重新提取 == 基线（逐字节）；
  2. 换算常量：x 1.875 = 1920/1024、y 1.6875 = 1080/640，float 位精确；
  3. helper 家族 5 个，syscall 号 86/89/90/94/110；
  4. 四个窗口：552/8、551/0、558/10、553/9，三个字面量窗口的画布坐标；
  5. 正文窗口的设计坐标来自局部变量默认值 230 / 522（不是字面量直传）；
  6. TextBuff 钳位 1024；
  7. C 侧交叉核对：syscall case 86 写 X/Y（否则脚本算好的画布坐标会被丢掉）。

缺游戏数据（HCB）时 SKIP 并返回 0。

用法：python tests/verify_script_window_geometry.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-script-window-geometry.py')
REF = os.path.join(FVP, 'ref', 'script-window-geometry.json')
HCB = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                   '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD', 'iroseka.hcb')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read(path):
    return io.open(path, encoding='utf-8').read()


def main():
    if not os.path.isfile(HCB):
        print('SKIP: 缺 HCB 脚本 %s（游戏数据不入库）' % HCB)
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
          'extract-script-window-geometry.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    ref = json.loads(read(REF))
    sc = ref.get('script_coords') or {}

    print('== 2. 设计坐标 → 画布坐标 ==')
    check(sc.get('design') == [1024, 640],
          '设计空间 1024x640（实际 %r）' % (sc.get('design'),))
    check(sc.get('canvas') == [1920, 1080],
          '画布 1920x1080（实际 %r）' % (sc.get('canvas'),))
    check(abs(sc.get('scale_x', 0) - 1.875) < 1e-9,
          'x 比例 = 1.875（实际 %r）' % (sc.get('scale_x'),))
    check(abs(sc.get('scale_y', 0) - 1.6875) < 1e-9,
          'y 比例 = 1.6875（实际 %r）' % (sc.get('scale_y'),))
    check(sc.get('float_bits') == {'x': '0x3ff00000', 'y': '0x3fd80000'},
          'float 立即数 x=0x3FF00000 / y=0x3FD80000（实际 %r）'
          % (sc.get('float_bits'),))
    check(abs(sc['scale_x'] - 1920.0 / 1024.0) < 1e-9,
          '1.875 == 1920/1024')
    check(abs(sc['scale_y'] - 1080.0 / 640.0) < 1e-9,
          '1.6875 == 1080/640')

    print('== 3. 窗口 helper 家族 ==')
    helpers = {h['syscall']: h for h in ref.get('helpers') or []}
    check(sorted(helpers) == [86, 89, 90, 94, 110],
          'helper 覆盖 syscall 86/89/90/94/110（实际 %r）'
          % (sorted(helpers),))
    check(helpers.get(86, {}).get('pc') == '0x035c10',
          'PrimSetText helper 在 0x035c10')

    print('== 4. 四个文本窗口的绑定 ==')
    want = {(552, 8): 'name', (551, 0): 'body', (558, 10): 'overlay',
            (553, 9): 'chapter'}
    got = {}
    for w in ref.get('windows') or []:
        got[(w['prim'], w['layer'])] = w
    check(set(got) == set(want),
          '绑定集合 = %r（实际 %r）' % (sorted(want), sorted(got)))
    for key, role in want.items():
        check(got.get(key, {}).get('role') == role,
              '%s：prim %d 绑 layer %d' % (role, key[0], key[1]))
    check(got[(552, 8)].get('canvas') == {'x': 375, 'y': 852},
          '名字窗口画布 (375, 852)（实际 %r）' % (got[(552, 8)].get('canvas'),))
    check(got[(553, 9)].get('canvas') == {'x': 165, 'y': 590},
          '章节标题画布 (165, 590)（实际 %r）'
          % (got[(553, 9)].get('canvas'),))
    check(got[(558, 10)].get('canvas') == {'x': 0, 'y': 0},
          '覆盖窗画布 (0, 0)')
    check('canvas' not in got[(551, 0)],
          '正文窗口的坐标不是字面量（运行时取局部变量）')

    print('== 5. 正文窗口的默认值 ==')
    defaults = {d['local']: d['value']
                for d in ref.get('body_window_defaults') or []}
    check(defaults == {250: 230, 251: 522},
          '正文窗口默认设计坐标 (230, 522)（实际 %r）' % (defaults,))

    print('== 6. TextBuff 钳位 ==')
    clamp = ref.get('textbuff_clamp') or {}
    check(clamp.get('width') == 1024 and clamp.get('height') == 1024,
          'TextBuff 宽高各自钳到 1024（实际 %r, %r）'
          % (clamp.get('width'), clamp.get('height')))
    check(clamp.get('pc') == '0x035da0', '钳位指令在 0x035da0')

    print('== 7. C 侧交叉核对 ==')
    sysc = read(os.path.join(FVP, 'src', 'syscall.c'))
    seg = sysc[sysc.index('case 86:'):sysc.index('case 87:')]
    check('GFX_F_X' in seg and 'GFX_F_Y' in seg,
          'syscall case 86 写入 prim 的 X/Y（脚本算好的画布坐标不能丢）')
    check('fvp_gfx_is_int' in seg,
          'case 86 只在实参是整数时写 X/Y（原版 handler 0x43BDC0 的 type==2 判断）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 脚本侧窗口几何（设计 1024x640 → 画布 1920x1080）× 四个窗口'
          '与 exe/脚本一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())