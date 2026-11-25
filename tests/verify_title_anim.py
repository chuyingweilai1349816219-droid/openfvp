#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题画面/开场动画的门禁：参数必须等于脚本里的常量，且**槽位要对**。

⚠️ 2026-11-25 修正：上一版门禁把「alpha 0→255 / 1400 ms + 缩放 10000→1000」当成
**标题 logo** 的进场动画，是错的。按 hcb 的加载顺序复核后：

  * 250 `logo_bg` / 252 `logo_favo` / 254 `logo_favo_view` / 255..257
    `logo_favo_view_p1..p3` 是**开场 FAVORITE logo**（hcb 0x580AA..0x581C4 加载），
    缩放 10→1 / 1400 ms 属于其中的 p1..p3（hcb 0x58217..0x582EC）；
  * **标题画面**是 265 `title_logo1`（0x58B72 加载）+ 262..264 与菜单项，
    进场动画是 `MotionAlpha(0, 255, <local5=500>)`（hcb 0x59349..0x595C1）——
    **500 ms 淡入**；背景四层同时做 25 秒的极慢缩放（hcb 0x59143..，时长 local7=25000）。

本门禁断言：

  1. `ref/title-scene.json` 里：logo_favo_view_p1..p3 的 MotionAlpha/MotionMoveS2 参数；
     标题 262..265 的淡入条目（0→255，时长是栈局部变量 5/7，说明"时长由运行期设置"）；
     背景 258 的 MotionMoveS2 = 1300→900（时长栈局部 7）；
  2. `src/titleanim.h` 的宏与上面逐项一致（500 / 25000 / 10000→1000/1400 …）；
  3. `main_win32.c` **只对标题做 alpha 淡入**（用 `gfx_draw_image_alpha`），
     不再把缩放动画挂到标题 logo 上（`gfx_draw_image_xform` 不应出现在标题绘制里），
     默认 title_fade = 255（冻结基线不变），`--title-anim`/`--opening` 才打开。

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

OPENING_VIEW_PRIMS = (255, 256, 257)
TITLE_FADE_PCS = {'0x00059349': 264, '0x00059361': 265, '0x00059370': 263,
                  '0x00059394': 262}

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def macros(path):
    text = io.open(path, encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'^#define\s+(FVP_\w+)\s+(-?(?:0x[0-9a-fA-F]+|\d+))\s*$',
                         text, re.M):
        out[m.group(1)] = int(m.group(2), 0)
    return out


def motion(entries, prim, name, pc=None):
    for e in entries:
        if e.get('prim') != prim or e.get('name') != name:
            continue
        if pc is not None and e.get('pc') != pc:
            continue
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
    entries = ((ref.get('games') or {}).get('iroseka') or {}).get(
        'layer_motion') or []
    check(len(entries) > 0, 'layer_motion 非空（实际 %d 条）' % len(entries))

    print('== 1. 开场 FAVORITE logo（255/256/257 = logo_favo_view_p1..p3）==')
    for prim in OPENING_VIEW_PRIMS:
        a = motion(entries, prim, 'MotionAlpha')
        s = motion(entries, prim, 'MotionMoveS2')
        check(a is not None and a['args'][1] == 0 and a['args'][2] == 255
              and a['args'][3] == 1400,
              '图元 %d 的 MotionAlpha = 0→255/1400 ms（实际 %r）'
              % (prim, a['args'] if a else None))
        check(s is not None and s['args'][1:5] == [10000, 1000, 10000, 1000]
              and s['args'][5] == 1400 and s['args'][6] == 3,
              '图元 %d 的 MotionMoveS2 = 10000→1000 两轴 / 1400 ms / mode 3（实际 %r）'
              % (prim, s['args'] if s else None))

    print('== 2. 标题画面：262..265 是 MotionAlpha 0→255，时长来自栈局部变量 ==')
    durations = set()
    for pc, prim in TITLE_FADE_PCS.items():
        e = motion(entries, prim, 'MotionAlpha', pc)
        if e is None:
            # pc 字符串格式可能带前导零差异
            e = next((x for x in entries
                      if x.get('name') == 'MotionAlpha' and x.get('prim') == prim
                      and x.get('pc', '').lower().endswith(pc[-5:])), None)
        check(e is not None, '图元 %d 在 %s 有 MotionAlpha' % (prim, pc))
        if e is not None:
            # 264 的 from 是 `pushtrue`（非 int）——按 handler 的缺省语义表示
            # "从当前 alpha 开始"，其它三个是明确的 0。
            check(e['args'][1] in (0, None) and e['args'][2] == 255,
                  '图元 %d 的 MotionAlpha 是 (0 或当前值)→255（实际 %r）'
                  % (prim, e['args']))
            durations.add(e['args'][3])
    check(durations <= {5, 7},
          '标题淡入的时长参数是栈局部变量 5 或 7（说明由运行期决定，实际 %r）'
          % sorted(x for x in durations if isinstance(x, int)))

    print('== 3. 背景 258..261：MotionMoveS2 1300→900（时长栈局部 7 = 25000 ms）==')
    e = motion(entries, 258, 'MotionMoveS2')
    check(e is not None, '图元 258 有 MotionMoveS2')
    if e is not None:
        check(e['args'][1] == 1300 and e['args'][2] == 900,
              '258 的缩放 1300→900（实际 %r）' % (e['args'],))
        check(e['args'][5] == 7,
              '时长参数是栈局部 7（实际 %r）' % (e['args'][5],))

    print('== 4. src/titleanim.h 的宏与脚本一致 ==')
    m = macros(HEADER)
    check(m.get('FVP_TITLEANIM_FADE_FROM') == 0 and
          m.get('FVP_TITLEANIM_FADE_TO') == 255,
          '标题淡入 0→255（实际 %r/%r）'
          % (m.get('FVP_TITLEANIM_FADE_FROM'), m.get('FVP_TITLEANIM_FADE_TO')))
    check(m.get('FVP_TITLEANIM_FADE_MS') == 500,
          '标题淡入时长 = 500 ms（脚本 local5，实际 %r）'
          % (m.get('FVP_TITLEANIM_FADE_MS'),))
    check(m.get('FVP_TITLEANIM_BG_SCALE_FROM') == 1300 and
          m.get('FVP_TITLEANIM_BG_SCALE_TO') == 900,
          '背景缩放 1300→900（实际 %r/%r）'
          % (m.get('FVP_TITLEANIM_BG_SCALE_FROM'),
             m.get('FVP_TITLEANIM_BG_SCALE_TO')))
    check(m.get('FVP_TITLEANIM_BG_MS') == 25000,
          '背景缩放时长 = 25000 ms（脚本 local7，实际 %r）'
          % (m.get('FVP_TITLEANIM_BG_MS'),))
    check(m.get('FVP_OPENANIM_VIEW_SCALE_FROM') == 10000 and
          m.get('FVP_OPENANIM_VIEW_SCALE_TO') == 1000 and
          m.get('FVP_OPENANIM_VIEW_MS') == 1400,
          '开场 p1..p3 缩放 10000→1000 / 1400 ms（实际 %r/%r/%r）'
          % (m.get('FVP_OPENANIM_VIEW_SCALE_FROM'),
             m.get('FVP_OPENANIM_VIEW_SCALE_TO'),
             m.get('FVP_OPENANIM_VIEW_MS')))
    check(m.get('FVP_OPENANIM_LOGO_BG_MS') == 1200 and
          m.get('FVP_OPENANIM_LOGO_FAVO_MS') == 1400,
          '开场 logo_bg 1200 ms / logo_favo 1400 ms 淡入（实际 %r/%r）'
          % (m.get('FVP_OPENANIM_LOGO_BG_MS'),
             m.get('FVP_OPENANIM_LOGO_FAVO_MS')))

    print('== 5. main_win32.c：标题只做 alpha 淡入，不再用缩放 ==')
    src = io.open(MAIN, encoding='utf-8').read()
    check('FVP_TITLEANIM_FADE_MS' in src, '引用 FVP_TITLEANIM_FADE_MS')
    check('gfx_draw_image_alpha(' in src,
          '标题用 gfx_draw_image_alpha 做淡入')
    check('gfx_draw_image_xform(' not in src,
          '标题绘制不再调用 gfx_draw_image_xform（缩放动画不属于标题）')
    check(re.search(r'static int title_fade = 255', src) is not None,
          '默认 title_fade = 255（冻结基线不变）')
    check(re.search(r'opts\.title_anim\s*=\s*opts\.opening\s*\?\s*1\s*:\s*0', src)
          is not None,
          '只有 --opening（或显式 --title-anim）才打开动画')
    check('fade-in started' in src, '日志写明这是淡入')
    check('0x58B72' in io.open(HEADER, encoding='utf-8').read() or
          '0x58b72' in io.open(HEADER, encoding='utf-8').read(),
          '头文件注明了标题 logo（265）的加载点 0x58B72')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题 = 500 ms 淡入（262..265/菜单），背景 = 25000 ms 慢缩放；'
          '缩放 10→1/1400 ms 属于开场 FAVORITE logo 的 p1..p3')
    return 0


if __name__ == '__main__':
    sys.exit(main())