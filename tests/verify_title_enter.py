#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题进场逐元素时间轴的门禁（两个作品）：C 侧常量必须等于脚本里读出来的事实。

为什么要有它：`src/titleanim.h` 以前把标题进场写成"统一的 500 ms 淡入"。
那个 500 来自 hcb 0x59120 的 local5，而那一处只被**背景动画**用到；标题对象真正
的进场在 0x592C7 被重设成 local5=2000 / local7=3000，菜单项是 local5=900 /
local7=820。`scripts/extract-title-enter.py` 把两个作品的进场块解成
`ref/title-enter.json`（五彩斑斓 0x5933E..0x59650、星空 0x6FEDE..0x701E2），
本门禁把它和 C 侧冻结：

    | | 五彩斑斓 | 星空 |
    |---|---|---|
    | logo 起点/时长 | 2000 / 2000 | 1000 / 2000 |
    | 菜单起点 | 2400 | 1400 |
    | 菜单时长 | 首项 900、其余 820、步进 40 | 同 |
    | 菜单初位移 | +23 px | +50 px |
    | 版权行 起点/时长/位移 | 2560 / 820 / +30 px | 1560 / 820 / +76 px |
    | 总时长 | 4000 ms | 3000 ms |

另外断言：装饰层参数的冻结、五彩斑斓 1522/1529 的 global3 分支特例只记录不实现、
`src/titleenter.c` 的每作品表、`main_win32.c` 的用法与默认静态。

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
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}

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
    missing = [n for n, p in GAMES.items() if not os.path.isfile(p)]
    if missing:
        print('SKIP: 缺游戏数据 %s（可选语料，见 docs/status.md）' % missing)
        return 0
    for path in (REF, HEADER, MODULE, MAIN):
        if not os.path.isfile(path):
            print('FAIL: 缺 %s' % path)
            return 1

    print('== 1. 重新提取并与基线逐字节比对（两个作品）==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-title-enter.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    hdr = defines(io.open(HEADER, encoding='utf-8').read())

    def h(name):
        return hdr.get(name)

    print('== 2. 两作共用的时长/步进 ==')
    check(h('FVP_TITLEENTER_LOGO_MS') == 2000, 'logo 2000 ms')
    check(h('FVP_TITLEENTER_MENU_FIRST_MS') == 900, '菜单首项 900 ms')
    check(h('FVP_TITLEENTER_MENU_MS') == 820, '菜单其余 820 ms')
    check(h('FVP_TITLEENTER_MENU_STEP_MS') == 40, '菜单步进 40 ms')
    check(h('FVP_TITLEENTER_MENU_COUNT') == 5, '菜单 5 项')
    check(h('FVP_TITLEENTER_COPY_MS') == 820, '版权行 820 ms')

    expect = {
        'iroseka': {
            'logo_start': 'FVP_TITLEENTER_IROSEKA_LOGO_START_MS',
            'menu_start': 'FVP_TITLEENTER_IROSEKA_MENU_START_MS',
            'copy_start': 'FVP_TITLEENTER_IROSEKA_COPY_START_MS',
            'total': 'FVP_TITLEENTER_IROSEKA_TOTAL_MS',
            'menu_dy': 'FVP_TITLEENTER_IROSEKA_MENU_DY',
            'copy_dx': 'FVP_TITLEENTER_IROSEKA_COPY_DX',
            'menus': [1501, 1508, 1515, 1522, 1529],
            'frames': [262, 263, 264],
        },
        'hoshimemo': {
            'logo_start': 'FVP_TITLEENTER_HOSHIMEMO_LOGO_START_MS',
            'menu_start': 'FVP_TITLEENTER_HOSHIMEMO_MENU_START_MS',
            'copy_start': 'FVP_TITLEENTER_HOSHIMEMO_COPY_START_MS',
            'total': 'FVP_TITLEENTER_HOSHIMEMO_TOTAL_MS',
            'menu_dy': 'FVP_TITLEENTER_HOSHIMEMO_MENU_DY',
            'copy_dx': 'FVP_TITLEENTER_HOSHIMEMO_COPY_DX',
            'menus': [266, 268, 270, 272, 274],
            'frames': [259, 260],
        },
    }

    for name in sorted(expect):
        e = expect[name]
        g = ref['games'][name]
        el = g['elements']
        print('== 3. %s：%s..%s ==' % (name, g['entrance_start'],
                                       g['entrance_end']))

        def alpha(prim):
            return (el.get(str(prim)) or {}).get('alpha') or {}

        def move(prim):
            return (el.get(str(prim)) or {}).get('move') or {}

        check(g['total_ms_drawn'] == h(e['total']),
              '只算引擎会画的元素时总时长 = %s ms == %s'
              % (g['total_ms_drawn'], e['total']))

        # logo（星空有两层 logo：265 + 276，都按同一时间轴）
        logo_prims = [265]
        if name == 'hoshimemo':
            logo_prims.append(276)
        for prim in logo_prims:
            check(alpha(prim).get('start_ms') == h(e['logo_start']),
                  'logo(%d) 起点 %s ms' % (prim, alpha(prim).get('start_ms')))
            check(alpha(prim).get('duration_ms') == h('FVP_TITLEENTER_LOGO_MS'),
                  'logo(%d) 时长 %s ms' % (prim, alpha(prim).get('duration_ms')))

        # 菜单
        first = e['menus'][0]
        check(alpha(first).get('start_ms') == h(e['menu_start']),
              '菜单首项起点 %s ms' % alpha(first).get('start_ms'))
        check(alpha(first).get('duration_ms') ==
              h('FVP_TITLEENTER_MENU_FIRST_MS'),
              '菜单首项时长 %s ms' % alpha(first).get('duration_ms'))
        for idx, prim in enumerate(e['menus']):
            want = h(e['menu_start']) + idx * h('FVP_TITLEENTER_MENU_STEP_MS')
            check(alpha(prim).get('start_ms') == want,
                  '菜单(%d) 起点 %s ms（步进 %d）'
                  % (prim, alpha(prim).get('start_ms'), idx))
            if idx > 0:
                check(alpha(prim).get('duration_ms') ==
                      h('FVP_TITLEENTER_MENU_MS'),
                      '菜单(%d) 时长 %s ms' % (prim,
                                              alpha(prim).get('duration_ms')))
            check(move(prim).get('from_dy') == h(e['menu_dy']) and
                  move(prim).get('to_dy') == 0,
                  '菜单(%d) 纵向 +%s px → 0'
                  % (prim, move(prim).get('from_dy')))

        # 版权行
        check(alpha(278).get('start_ms') == h(e['copy_start']),
              '版权行起点 %s ms' % alpha(278).get('start_ms'))
        check(alpha(278).get('duration_ms') == h('FVP_TITLEENTER_COPY_MS'),
              '版权行时长 %s ms' % alpha(278).get('duration_ms'))
        check(move(278).get('from_dx') == h(e['copy_dx']) and
              move(278).get('to_dx') == 0,
              '版权行横向 +%s px → 0' % move(278).get('from_dx'))

        # 装饰层只冻结、不绘制
        for prim in e['frames']:
            check(el[str(prim)]['drawn_by_engine'] is False,
                  '装饰层 %d 标记为未绘制' % prim)
        if name == 'iroseka':
            check(alpha(264).get('start_ms') ==
                  h('FVP_TITLEENTER_FRAME_START_MS') and
                  alpha(263).get('duration_ms') ==
                  h('FVP_TITLEENTER_FRAME_MS') and
                  alpha(262).get('duration_ms') ==
                  h('FVP_TITLEENTER_FRAME2_MS'),
                  '装饰层 264/263/262 的时长与头文件一致')
            check(abs(move(263).get('from_dy')) ==
                  h('FVP_TITLEENTER_SLIDE_263'),
                  '装饰层 263 纵向 %s px → 0' % move(263).get('from_dy'))
            alt = (el['1522'].get('move_alternatives') or [])
            check(len(alt) == 1 and alt[0].get('to_dy') == -81,
                  '1522 的 global3 分支（终点 -81 px）只记录、不实现')

    print('== 4. C 侧模块与外壳用法 ==')
    mod = io.open(MODULE, encoding='utf-8').read()
    check('fvp_titleenter_alpha' in mod and 'fvp_titleenter_offset' in mod,
          'titleenter.c 提供 alpha / offset')
    check('GAMES[]' in mod and '"iroseka"' in mod and '"hoshimemo"' in mod,
          'titleenter.c 有每作品表')
    check('FVP_TITLEENTER_IROSEKA_MENU_DY' in mod and
          'FVP_TITLEENTER_HOSHIMEMO_MENU_DY' in mod,
          '每作品的位移走 titleanim.h 的宏')
    main_c = io.open(MAIN, encoding='utf-8').read()
    check('fvp_titleenter_alpha' in main_c and
          'fvp_titleenter_offset' in main_c,
          'main_win32.c 用逐元素 alpha/offset')
    check('fvp_titleenter_game_from_name' in main_c,
          'main_win32.c 按作品取进场时间轴')
    check('title_enter_reset' in main_c and
              re.search(r'title_logo_alpha = 255', main_c) is not None,
          '默认/结束时复位成 255（静态画面逐字节不变）')
    check('static int title_fade = ' not in main_c,
          '旧的统一 title_fade 已删除')
    check('FVP_TITLEANIM_FADE_' not in
          io.open(HEADER, encoding='utf-8').read(),
          '旧的 500 ms 常量已删除')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题进场 == 脚本的逐元素时间轴（五彩斑斓 2000/2400/2560、总计 4000；'
          '星空 1000/1400/1560、总计 3000）')
    return 0


if __name__ == '__main__':
    sys.exit(main())