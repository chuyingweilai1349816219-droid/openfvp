#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题画面进场动画的门禁：参数必须等于脚本里的常量，且**槽位要对**。

⚠️ 2026-11-25 修正：上一版门禁把「alpha 0→255 / 1400 ms + 缩放 10000→1000」当成
**标题 logo** 的进场动画，是错的。按 hcb 的加载顺序复核后：

  * 250 `logo_bg` / 252 `logo_favo` / 254 `logo_favo_view` / 255..257
    `logo_favo_view_p1..p3` 是**开场 FAVORITE logo**（hcb 0x580AA..0x581C4 加载），
    缩放 10→1 / 1400 ms 属于其中的 p1..p3（hcb 0x58217..0x582EC）；
    这套参数现在冻结在 `ref/opening-scene.json` + `src/openinganim.h`，门禁是
    `tests/verify_opening_anim.py`（本文件不再重复断言开场）；
  * **标题画面**是 265 `title_logo1`（0x58B72 加载）+ 262..264 与菜单项；
    进场是**逐元素错开**的时间轴（hcb 0x5933E..0x59650），现在冻结在
    `ref/title-enter.json` + `src/titleenter.h`，门禁是
    `tests/verify_title_enter.py`（本文件不再重复断言进场）；
    背景四层同时做 25 秒的极慢缩放（hcb 0x59143..，时长 local7=25000）。

本门禁断言：

  1. `ref/title-scene.json` 里：标题 262..265 的淡入条目（0→255，时长是栈局部
     变量 5/7，说明"时长由运行期设置"）；背景 258 的 MotionMoveS2 = 1300→900
     （时长栈局部 7）；
  2. `src/titleanim.h` 不再有"统一 500 ms 淡入"常量，只剩背景 25000 ms
     与进场逐元素常量（FVP_TITLEENTER_*）；开场常量在 `src/openinganim.h`；
  3. `main_win32.c` 用逐元素 alpha 画标题（默认静态 = 255，`--title-anim`/
     `--opening` 才打开），没有把缩放动画挂到标题 logo 上。

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

    print('== 1. 开场参数已经独立成 openinganim.h / opening-scene.json ==')
    open_h = os.path.join(FVP, 'src', 'openinganim.h')
    open_ref = os.path.join(FVP, 'ref', 'opening-scene.json')
    check(os.path.isfile(open_h) and os.path.isfile(open_ref),
          '开场参数在 src/openinganim.h + ref/opening-scene.json')
    if os.path.isfile(open_h):
        text = io.open(open_h, encoding='utf-8').read()
        check(re.search(r'FVP_OPEN_TOTAL_MS\s+3800', text) is not None,
              '开场总时长 3800 ms（脚本 1200+2500+100，旧拟合值 5400 已作废）')
        check(re.search(r'FVP_OPEN_VIEW_SCALE_FROM\s+10000', text) is not None,
              '开场 p1..p3 缩放 10000→1000 在 openinganim.h')
    check('FVP_OPENANIM_' not in io.open(HEADER, encoding='utf-8').read(),
          'titleanim.h 不再定义开场常量')

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

    print('== 4. src/titleanim.h：背景常量在、统一 500 ms 淡入已删除 ==')
    m = macros(HEADER)
    check(m.get('FVP_TITLEANIM_BG_SCALE_FROM') == 1300 and
          m.get('FVP_TITLEANIM_BG_SCALE_TO') == 900,
          '背景缩放 1300→900（实际 %r/%r）'
          % (m.get('FVP_TITLEANIM_BG_SCALE_FROM'),
             m.get('FVP_TITLEANIM_BG_SCALE_TO')))
    check(m.get('FVP_TITLEANIM_BG_MS') == 25000,
          '背景缩放时长 = 25000 ms（脚本 local7，实际 %r）'
          % (m.get('FVP_TITLEANIM_BG_MS'),))
    check(not [k for k in m if k.startswith('FVP_OPENANIM_')],
          'titleanim.h 里没有开场常量（开场由 openinganim.h 负责）')
    check(not [k for k in m if k.startswith('FVP_TITLEANIM_FADE_')],
          '统一 500 ms 淡入常量已删除（逐元素时间轴在 FVP_TITLEENTER_*，'
          '由 tests/verify_title_enter.py 门禁）')
    check(m.get('FVP_TITLEENTER_IROSEKA_TOTAL_MS') == 4000 and
          m.get('FVP_TITLEENTER_HOSHIMEMO_TOTAL_MS') == 3000,
          '标题进场总时长 = 五彩 4000 / 星空 3000 ms（两作进场起点不同）')

    print('== 5. main_win32.c：标题用逐元素进场，默认静态 ==')
    src = io.open(MAIN, encoding='utf-8').read()
    check('FVP_TITLEENTER_LOGO_MS' in src, '引用 FVP_TITLEENTER_LOGO_MS')
    check('gfx_draw_image_alpha(' in src,
          '标题用 gfx_draw_image_alpha 做淡入')
    check('gfx_draw_image_xform(' not in src,
          '标题绘制不再调用 gfx_draw_image_xform（缩放动画不属于标题）')
    check('title_enter_reset' in src,
          '默认/结束时复位成静态画面（冻结基线不变）')
    check(re.search(r'opts\.title_anim\s*=\s*opts\.opening\s*\?\s*1\s*:\s*0', src)
          is not None,
          '只有 --opening（或显式 --title-anim）才打开动画')
    check('script entrance started' in src, '日志写明这是脚本进场')
    check('0x58B72' in io.open(HEADER, encoding='utf-8').read() or
          '0x58b72' in io.open(HEADER, encoding='utf-8').read(),
          '头文件注明了标题 logo（265）的加载点 0x58B72')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题场景事实 + 背景 25000 ms 常量；标题进场是逐元素脚本时间轴'
          '（见 tests/verify_title_enter.py）')
    return 0


if __name__ == '__main__':
    sys.exit(main())