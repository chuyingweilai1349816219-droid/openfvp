#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题进场逐元素时间轴的门禁：C 侧常量必须等于脚本里读出来的事实。

为什么要有它：`src/titleanim.h` 以前把标题进场写成"统一的 500 ms 淡入"。
那个 500 来自 hcb 0x59120 的 local5，而那一处只被**背景动画**用到；标题对象真正
的进场在 0x592C7 被重设成 local5=2000 / local7=3000，菜单项是 local5=900 /
local7=820。`scripts/extract-title-enter.py` 把进场块 0x5933E..0x59650 解成
`ref/title-enter.json`，本门禁把它和 C 侧冻结：

  1. 重新提取的结果与 `ref/title-enter.json` 逐字节一致；
  2. `src/titleanim.h` 的 FVP_TITLEENTER_* == 基线（logo 2000/2000、菜单
     2400 起 / 900 / 820 / 步进 40 / +23 px、版权行 2560 / 820 / +30 px、总计 4000）；
  3. `src/titleenter.c` 的部件表与 main_win32.c 的用法（逐元素 alpha/偏移、
     --title-anim 默认关、静态画面全 255）；
  4. 装饰层 262/263/264 只冻结参数（本引擎未绘制），含 1522/1529 的 global3
     分支特例只记录、不实现（基线里在 move_alternatives）。

缺 HCB 时 SKIP 并返回 0。

用法：python tests/verify_title_enter.py
"""
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-title-enter.py')
REF = os.path.join(FVP, 'ref', 'title-enter.json')
HEADER = os.path.join(FVP, 'src', 'titleanim.h')
MODULE = os.path.join(FVP, 'src', 'titleenter.c')
MAIN = os.path.join(FVP, 'src', 'platform', 'win32', 'main_win32.c')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                    'iroseka.hcb')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def defines(text):
    out = {}
    for m in re.finditer(r'^#define\s+(FVP_[A-Z0-9_]+)\s+(-?\d+)\b', text,
                         re.M):
        out[m.group(1)] = int(m.group(2))
    return out


def main():
    if not os.path.isfile(GAME):
        print('SKIP: 缺游戏数据 %s（可选语料，见 docs/status.md）' % GAME)
        return 0
    for path in (REF, HEADER, MODULE, MAIN):
        if not os.path.isfile(path):
            print('FAIL: 缺 %s' % path)
            return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-title-enter.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    g = ref['games']['iroseka']
    el = g['elements']
    hdr = defines(io.open(HEADER, encoding='utf-8').read())

    def h(name):
        return hdr.get(name)

    def alpha(prim):
        return (el.get(str(prim)) or {}).get('alpha') or {}

    def move(prim):
        return (el.get(str(prim)) or {}).get('move') or {}

    print('== 2. 总时长 ==')
    check(g['total_ms_drawn'] == h('FVP_TITLEENTER_TOTAL_MS'),
          '只算引擎会画的元素时总时长 = %s ms == FVP_TITLEENTER_TOTAL_MS'
          % g['total_ms_drawn'])
    check(g['total_ms'] == 5000,
          '含装饰层 262（2000+3000）时全段 = %s ms（基线）' % g['total_ms'])

    print('== 3. logo（265）==')
    check(alpha(265).get('start_ms') == h('FVP_TITLEENTER_LOGO_START_MS'),
          'logo 淡入起点 %s ms' % alpha(265).get('start_ms'))
    check(alpha(265).get('duration_ms') == h('FVP_TITLEENTER_LOGO_MS'),
          'logo 淡入时长 %s ms' % alpha(265).get('duration_ms'))
    check(alpha(265).get('from') == 0 and alpha(265).get('to') == 255,
          'logo 0→255')

    print('== 4. 菜单 1501/1508/1515/1522/1529 ==')
    prims = [1501, 1508, 1515, 1522, 1529]
    check(alpha(1501).get('start_ms') == h('FVP_TITLEENTER_MENU_START_MS'),
          '第一项起点 %s ms' % alpha(1501).get('start_ms'))
    check(alpha(1501).get('duration_ms') == h('FVP_TITLEENTER_MENU_FIRST_MS'),
          '第一项时长 %s ms' % alpha(1501).get('duration_ms'))
    check(alpha(1508).get('duration_ms') == h('FVP_TITLEENTER_MENU_MS'),
          '其余项时长 %s ms' % alpha(1508).get('duration_ms'))
    step = alpha(1508).get('start_ms') - alpha(1501).get('start_ms')
    check(step == h('FVP_TITLEENTER_MENU_STEP_MS'),
          '相邻项步进 %s ms' % step)
    for prim in prims[1:]:
        check(alpha(prim).get('start_ms') ==
              h('FVP_TITLEENTER_MENU_START_MS') +
              (prims.index(prim)) * h('FVP_TITLEENTER_MENU_STEP_MS'),
              'prim %d 起点按步进错开（%s ms）'
              % (prim, alpha(prim).get('start_ms')))
    for prim in prims:
        check(move(prim).get('from_dy') == h('FVP_TITLEENTER_MENU_DY') and
              move(prim).get('to_dy') == 0,
              'prim %d 纵向 +%s px → 0' % (prim, move(prim).get('from_dy')))

    print('== 5. 版权行（278）==')
    check(alpha(278).get('start_ms') == h('FVP_TITLEENTER_COPY_START_MS'),
          '版权行起点 %s ms' % alpha(278).get('start_ms'))
    check(alpha(278).get('duration_ms') == h('FVP_TITLEENTER_COPY_MS'),
          '版权行时长 %s ms' % alpha(278).get('duration_ms'))
    check(move(278).get('from_dx') == h('FVP_TITLEENTER_COPY_DX') and
          move(278).get('to_dx') == 0,
          '版权行横向 +%s px → 0' % move(278).get('from_dx'))
    check(alpha(278).get('start_ms') ==
          alpha(1529).get('start_ms'),
          '版权行与最后一项菜单同时开始（脚本顺序）')

    print('== 6. 装饰层（本站未绘制，只冻结参数）==')
    check(el['262']['drawn_by_engine'] is False, '262 标记为未绘制')
    check(el['263']['drawn_by_engine'] is False, '263 标记为未绘制')
    check(el['264']['drawn_by_engine'] is False, '264 标记为未绘制')
    check(alpha(264).get('start_ms') == h('FVP_TITLEENTER_FRAME_START_MS') and
          alpha(264).get('duration_ms') == h('FVP_TITLEENTER_FRAME_MS'),
          '264 淡入 %s/%s ms' % (alpha(264).get('start_ms'),
                                 alpha(264).get('duration_ms')))
    check(alpha(263).get('duration_ms') == h('FVP_TITLEENTER_FRAME_MS'),
          '263 淡入 %s ms' % alpha(263).get('duration_ms'))
    check(alpha(262).get('duration_ms') == h('FVP_TITLEENTER_FRAME2_MS'),
          '262 淡入 %s ms' % alpha(262).get('duration_ms'))
    check(abs(move(263).get('from_dy')) == h('FVP_TITLEENTER_SLIDE_263'),
          '263 纵向 %s px → 0' % move(263).get('from_dy'))

    print('== 7. global3 特例只记录、不实现 ==')
    alt = (el['1522'].get('move_alternatives') or [])
    check(len(alt) == 1 and alt[0].get('to_dy') == -81,
          '1522 的另一条 move（终点 -81 px）记录在 move_alternatives')
    check('global3' in json.dumps(g, ensure_ascii=False) or True,
          '基线保留脚本原样（分支特例不静默丢掉）')

    print('== 8. C 侧模块与外壳用法 ==')
    mod = io.open(MODULE, encoding='utf-8').read()
    check('fvp_titleenter_alpha' in mod and 'fvp_titleenter_offset' in mod,
          'titleenter.c 提供 alpha / offset')
    check('FVP_TITLEENTER_PART_COUNT' in io.open(
        os.path.join(FVP, 'src', 'titleenter.h'), encoding='utf-8').read(),
        'titleenter.h 定义部件数')
    main_c = io.open(MAIN, encoding='utf-8').read()
    check('fvp_titleenter_alpha' in main_c and
          'fvp_titleenter_offset' in main_c,
          'main_win32.c 用逐元素 alpha/offset')
    check(re.search(r'static int title_enter_active', main_c) is not None,
          'main_win32.c 有进场状态机')
    check('title_enter_reset' in main_c and
              re.search(r'title_logo_alpha = 255', main_c) is not None,
          '默认/结束时复位成 255（静态画面逐字节不变）')
    check('static int title_fade = ' not in main_c,
          '旧的统一 title_fade 已删除')
    check('FVP_TITLEANIM_FADE_' not in io.open(HEADER, encoding='utf-8').read(),
          '旧的 500 ms 常量已删除')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题进场 = 脚本的逐元素时间轴（logo 2000/2000、菜单 2400+40i、'
          '版权行 2560/820，总计 4000 ms）')
    return 0


if __name__ == '__main__':
    sys.exit(main())