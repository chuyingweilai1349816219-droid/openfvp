#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题**进场动画**的门禁：C 里用的参数必须等于脚本里的常量。

问题：标题进场动画（logo 淡入 + 缩放）如果靠"看起来像"去调，永远调不出原版的
数字。这些数字在 hcb 脚本里是**常量**，已经冻结进 `ref/title-scene.json` 的
`games.iroseka.layer_motion`（由 `scripts/extract-title-scene.py` 静态提取，
`tests/verify_title_scene.py` 保证基线 = 脚本）。

本门禁把两边焊起来：

  1. 从 `ref/title-scene.json` 读 logo 三块（图元 255/256/257）的
     `MotionAlpha` 与 `MotionMoveS2`，断言三者参数**完全一致**；
  2. 解析 `src/titleanim.h` 里的宏，断言 from / to / dur / mode 与脚本逐项相等；
  3. 断言 `src/platform/win32/main_win32.c` 真的用了这些宏和
     `gfx_draw_image_xform()`，且**默认关闭**（`o->title_anim = -1`，
     只有 `--title-anim` 或 `--opening` 才打开）——默认关是冻结基线不被动的前提；
  4. 顺带断言背景四层（258..261）的 MotionMoveS2 是 7 ms（第 131 轮结论：
     那是"离开标题"的缩小，不是进场动画），别被误当成进场参数。

缺基线时 SKIP 并返回 0。

用法：python tests/verify_title_anim.py
"""
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'title-scene.json')
HEADER = os.path.join(FVP, 'src', 'titleanim.h')
MAIN = os.path.join(FVP, 'src', 'platform', 'win32', 'main_win32.c')

LOGO_PRIMS = (255, 256, 257)
BG_PRIMS = (258, 259, 260, 261)

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def macros(path):
    text = io.open(path, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^#define\s+(FVP_TITLEANIM_\w+)\s+'
                         r'(-?(?:0x[0-9a-fA-F]+|\d+))\s*$', text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def motion_of(entries, prim, name):
    for e in entries:
        if e.get('prim') == prim and e.get('name') == name:
            return e
    return None


def main():
    if not os.path.isfile(REF):
        print('SKIP: 缺基线 %s' % REF)
        return 0
    for p in (HEADER, MAIN):
        if not os.path.isfile(p):
            print('FAIL: 缺源文件 %s' % p)
            return 1

    ref = json.load(io.open(REF, encoding='utf-8'))
    game = (ref.get('games') or {}).get('iroseka') or {}
    entries = game.get('layer_motion') or []
    if not entries:
        print('SKIP: ref/title-scene.json 里没有 layer_motion')
        return 0

    print('== 1. 脚本里 logo 三块（255/256/257）的进场参数必须一致 ==')
    alpha = {}
    scale = {}
    for prim in LOGO_PRIMS:
        a = motion_of(entries, prim, 'MotionAlpha')
        s = motion_of(entries, prim, 'MotionMoveS2')
        check(a is not None, '图元 %d 有 MotionAlpha' % prim)
        check(s is not None, '图元 %d 有 MotionMoveS2' % prim)
        if a is not None:
            alpha[prim] = (a['args'][1], a['args'][2], a['args'][3])
        if s is not None:
            scale[prim] = (s['args'][1], s['args'][2], s['args'][3],
                           s['args'][4], s['args'][5], s['args'][6])
    check(len(set(alpha.values())) == 1,
          '三块的 MotionAlpha 参数一致（实际 %s）' % sorted(set(alpha.values())))
    check(len(set(scale.values())) == 1,
          '三块的 MotionMoveS2 参数一致（实际 %s）' % sorted(set(scale.values())))

    a0 = alpha.get(LOGO_PRIMS[-1])
    s0 = scale.get(LOGO_PRIMS[-1])
    print('== 2. 脚本参数（取自 iroseka 图元 257 的调用点）==')
    if a0 is not None:
        print('  MotionAlpha  : from=%r to=%r dur=%r' % a0)
    if s0 is not None:
        print('  MotionMoveS2 : sx %r->%r  sy %r->%r  dur=%r mode=%r' % s0)

    m = macros(HEADER)
    print('== 3. src/titleanim.h 的宏必须逐项等于脚本 ==')
    if a0 is not None:
        check(m.get('FVP_TITLEANIM_ALPHA_FROM') == a0[0],
              'ALPHA_FROM == %r（实际 %r）'
              % (a0[0], m.get('FVP_TITLEANIM_ALPHA_FROM')))
        check(m.get('FVP_TITLEANIM_ALPHA_TO') == a0[1],
              'ALPHA_TO == %r（实际 %r）'
              % (a0[1], m.get('FVP_TITLEANIM_ALPHA_TO')))
        check(m.get('FVP_TITLEANIM_DUR_MS') == a0[2],
              'DUR_MS == %r（实际 %r）'
              % (a0[2], m.get('FVP_TITLEANIM_DUR_MS')))
    if s0 is not None:
        check(m.get('FVP_TITLEANIM_SCALE_FROM') == s0[0] == s0[2],
              'SCALE_FROM == %r（两个轴同样，实际 %r）'
              % (s0[0], m.get('FVP_TITLEANIM_SCALE_FROM')))
        check(m.get('FVP_TITLEANIM_SCALE_TO') == s0[1] == s0[3],
              'SCALE_TO == %r（两个轴同样，实际 %r）'
              % (s0[1], m.get('FVP_TITLEANIM_SCALE_TO')))
        check(m.get('FVP_TITLEANIM_EASE_MODE') == s0[5],
              'EASE_MODE == %r（实际 %r）' % (s0[5],
                                              m.get('FVP_TITLEANIM_EASE_MODE')))
    check(m.get('FVP_TITLEANIM_LOGO_SLOT') == 0x0FF1,
          'LOGO_SLOT == 0x0FF1（实际 %r）' % m.get('FVP_TITLEANIM_LOGO_SLOT'))

    print('== 4. win32 外壳真的用了这些宏，且默认关闭 ==')
    src = io.open(MAIN, encoding='utf-8').read()
    for name in ('FVP_TITLEANIM_ALPHA_FROM', 'FVP_TITLEANIM_SCALE_FROM',
                 'FVP_TITLEANIM_DUR_MS', 'FVP_TITLEANIM_EASE_MODE'):
        check(name in src, 'main_win32.c 引用 %s' % name)
    check('gfx_draw_image_xform(' in src,
          'main_win32.c 用 gfx_draw_image_xform() 画动画中的 logo')
    check('fvp_motion_add(' in src,
          '用 src/motion.c 的补间（mode 1..5 曲线有静态分析依据）')
    check(re.search(r'o->title_anim = -1', src) is not None,
          '默认 title_anim = -1（未指定）')
    check(re.search(r'opts\.title_anim\s*=\s*opts\.opening\s*\?\s*1\s*:\s*0', src)
          is not None,
          '只有 --opening（或显式 --title-anim）才打开动画')
    check('TITLE_LOGO_SLOT 0x0FF1' in src,
          'main_win32.c 里 logo 槽就是 0x0FF1')

    print('== 5. 背景四层（258..261）不是进场动画（dur = 7 ms）==')
    durs = []
    for prim in BG_PRIMS:
        s = motion_of(entries, prim, 'MotionMoveS2')
        if s is not None:
            durs.append((prim, s['args'][5]))
    check(len(durs) == 4, '背景四层都有 MotionMoveS2（实际 %s）' % (durs,))
    check(all(d == 7 for _, d in durs),
          '四个时长都是 7 ms（"离开标题"的缩小，不是进场；实际 %s）' % (durs,))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题进场动画参数 = 脚本常量（alpha 0->255 / scale 10000->1000，'
          '1400 ms，mode 3），默认关闭，动画终点与静态绘制一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())