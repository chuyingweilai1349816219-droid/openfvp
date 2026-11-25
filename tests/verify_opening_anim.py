#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""开场 FAVORITE logo 序列门禁：C 侧常量必须等于脚本里读出来的事实。

为什么要有它：`--opening` 以前是一条**按抓帧时间线拟合**的时间轴
（白底 300 / 淡入 1200 / 停留 2100 / 淡出 1200 / 白场 600 = 5400 ms，源码注释写着
"时长与缓动是近似值"）。按项目方法论（先结构分析，不要先抓帧），这条时间轴必须从
`iroseka.hcb` / `Hoshimemo_HD.hcb` 的开场函数里读出来。`scripts/extract-opening-scene.py`
把这条时间轴（槽位、逐层 alpha、缩放、旋转、位移、OP 轴心）抽成
`ref/opening-scene.json`，本门禁把它们和 C 侧冻结：

  1. 重新提取的结果与 `ref/opening-scene.json` **逐字节一致**；
  2. `src/openinganim.h` 的每个常量 == 基线里的数字（两个作品共用的那部分）；
  3. `src/openseq.c` 的层表 = 脚本 GraphLoad 的六个槽位/名字、顺序一致；
  4. 每部作品的两处差异（逻辑画布 1024x640 / 800x600、p1..p3 的 OP y 628/610）
     在 C 里也是从文件里读出来的值；
  5. 外壳真的用这套时间轴画（`fvp_openseq_layer_state` + `gfx_draw_image_op_xform`），
     且开场期间把标题背景画在白底后面（脚本先建标题场景再跑开场）。

缺 HCB 时 SKIP 并返回 0（和 verify_title_scene.py 一致）。

用法：python tests/verify_opening_anim.py
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
EXTRACT = os.path.join(FVP, 'scripts', 'extract-opening-scene.py')
REF = os.path.join(FVP, 'ref', 'opening-scene.json')
HEADER = os.path.join(FVP, 'src', 'openinganim.h')
OPENSEQ = os.path.join(FVP, 'src', 'openseq.c')
MAIN = os.path.join(FVP, 'src', 'platform', 'win32', 'main_win32.c')
TITLEANIM = os.path.join(FVP, 'src', 'titleanim.h')
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


def read(path):
    with io.open(path, encoding='utf-8') as fp:
        return fp.read()


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
    for path in (REF, HEADER, OPENSEQ, MAIN):
        if not os.path.isfile(path):
            print('FAIL: 缺 %s' % path)
            return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-opening-scene.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    hdr = defines(read(HEADER))

    def h(name):
        return hdr.get(name)

    print('== 2. 两个作品共用的时间轴（脚本常量）==')
    for name in sorted(GAMES):
        tl = (ref['games'].get(name) or {}).get('timeline') or {}
        print('  -- %s：%s..%s' % (name, ref['games'][name]['opening_start'],
                                   ref['games'][name]['opening_end']))
        a_in = tl.get('alpha_in_ms') or {}
        check(a_in.get('252') == h('FVP_OPEN_ALPHA_MS_FAVO'),
              '%s logo_favo 淡入 = %s ms == FVP_OPEN_ALPHA_MS_FAVO'
              % (name, a_in.get('252')))
        for prim in ('254', '255', '256', '257'):
            check(a_in.get(prim) == h('FVP_OPEN_ALPHA_MS_VIEW'),
                  '%s prim %s 淡入 = %s ms == FVP_OPEN_ALPHA_MS_VIEW'
                  % (name, prim, a_in.get(prim)))
        a_out = tl.get('alpha_out_ms') or {}
        check(a_out.get('250') == h('FVP_OPEN_BG_OUT_MS'),
              '%s 白底淡出 = %s ms == FVP_OPEN_BG_OUT_MS'
              % (name, a_out.get('250')))
        for prim in ('252', '254', '255', '256', '257'):
            check(a_out.get(prim) == h('FVP_OPEN_LOGO_OUT_MS'),
                  '%s prim %s 淡出 = %s ms == FVP_OPEN_LOGO_OUT_MS'
                  % (name, prim, a_out.get(prim)))
        check(tl.get('out_start_ms') == h('FVP_OPEN_OUT_START_MS'),
              '%s 淡出起点 = %s ms（等 MotionAlphaTest(252)）'
              % (name, tl.get('out_start_ms')))
        check(tl.get('cut_ms') == h('FVP_OPEN_CUT_MS'),
              '%s 收尾 = %s ms' % (name, tl.get('cut_ms')))
        check(tl.get('cut_start_ms') == h('FVP_OPEN_CUT_START_MS'),
              '%s 收尾起点 = %s ms' % (name, tl.get('cut_start_ms')))
        check(tl.get('total_ms') == h('FVP_OPEN_TOTAL_MS'),
              '%s 总时长 = %s ms（旧拟合值是 5400）'
              % (name, tl.get('total_ms')))

        sc = (tl.get('scale') or {}).get('255') or {}
        check(sc.get('from_x') == h('FVP_OPEN_VIEW_SCALE_FROM') and
              sc.get('to_x') == h('FVP_OPEN_VIEW_SCALE_TO') and
              sc.get('ms') == h('FVP_OPEN_VIEW_MS') and
              sc.get('mode') == h('FVP_OPEN_EASE_MODE'),
              '%s p1..p3 缩放 %s->%s / %s ms / mode %s'
              % (name, sc.get('from_x'), sc.get('to_x'), sc.get('ms'),
                 sc.get('mode')))
        mz = (tl.get('move_z') or {}).get('252') or {}
        check(mz.get('from') == h('FVP_OPEN_SCALE_FROM_FAVO') and
              mz.get('to') == h('FVP_OPEN_SCALE_TO_FAVO') and
              mz.get('ms') == h('FVP_OPEN_SCALE_MS_FAVO'),
              '%s logo_favo 缩放 %s->%s / %s ms'
              % (name, mz.get('from'), mz.get('to'), mz.get('ms')))
        rot = (tl.get('rotate') or {}).get('255') or {}
        check(rot.get('from') == h('FVP_OPEN_VIEW_ANGLE_FROM') and
              rot.get('to') == h('FVP_OPEN_VIEW_ANGLE_TO') and
              rot.get('ms') == h('FVP_OPEN_VIEW_MS'),
              '%s p1..p3 旋转 %s->%s / %s ms'
              % (name, rot.get('from'), rot.get('to'), rot.get('ms')))

    print('== 3. 每部作品的两处差异 ==')
    for name in sorted(GAMES):
        g = ref['games'][name]
        tl = g['timeline']
        logical = g['logical']
        op = tl['op']
        print('  -- %s logical=%s op=%s' % (name, logical, op))
        if name == 'iroseka':
            check(logical['w'] == h('FVP_OPEN_LOGICAL_W_IROSEKA') and
                  logical['h'] == h('FVP_OPEN_LOGICAL_H_IROSEKA'),
                  'iroseka 逻辑画布 %sx%s' % (logical['w'], logical['h']))
            check(op['255'][1] == h('FVP_OPEN_OP_VIEW_Y_IROSEKA'),
                  'iroseka p1..p3 OP y = %s（脚本 PrimSetOP 字面量）'
                  % op['255'][1])
            mv = tl['move']
            check(mv['255']['from_x'] == -375 and mv['255']['from_y'] == 337,
                  'iroseka p1 位移起点 = (%d,%d)（逻辑 ±200 × 1.875/1.6875）'
                  % (mv['255']['from_x'], mv['255']['from_y']))
            check(mv['257']['from_y'] == -337, 'iroseka p3 dy = -337')
        else:
            check(logical['w'] == h('FVP_OPEN_LOGICAL_W_HOSHIMEMO') and
                  logical['h'] == h('FVP_OPEN_LOGICAL_H_HOSHIMEMO'),
                  'hoshimemo 逻辑画布 %sx%s' % (logical['w'], logical['h']))
            check(op['255'][1] == h('FVP_OPEN_OP_VIEW_Y_HOSHIMEMO'),
                  'hoshimemo p1..p3 OP y = %s（helper 的 339×1.8）'
                  % op['255'][1])
            mv = tl['move']
            check(mv['255']['from_x'] == -480 and mv['255']['from_y'] == 360,
                  'hoshimemo p1 位移起点 = (%d,%d)（逻辑 ±200 × 2.4/1.8）'
                  % (mv['255']['from_x'], mv['255']['from_y']))
        for prim, xy in (('252', (960, 540)), ('254', (960, 540))):
            check(op[prim] == list(xy),
                  '%s prim %s OP = %s（脚本 helper 的逻辑中心）'
                  % (name, prim, op[prim]))

    print('== 4. C 侧层表 / 游戏表 ==')
    seq = read(OPENSEQ)
    names = ['logo_bg', 'logo_favo', 'logo_favo_view', 'logo_favo_view_p1',
             'logo_favo_view_p2', 'logo_favo_view_p3']
    pos = [seq.find('"%s"' % n) for n in names]
    check(all(p >= 0 for p in pos) and pos == sorted(pos),
          'openseq.c 层表按脚本 GraphLoad 顺序包含六个素材名')
    for slot in ('250', '252', '254', '255', '256', '257'):
        check(('FVP_OPEN_SLOT_%s' % {'250': 'BG', '252': 'FAVO',
                                     '254': 'VIEW', '255': 'P1', '256': 'P2',
                                     '257': 'P3'}[slot]) in seq,
              '层表用了 FVP_OPEN_SLOT_%s' % slot)
    check('iroseka' in seq and 'hoshimemo' in seq,
          'openseq.c 有每作品表（iroseka / hoshimemo）')
    check('FVP_OPEN_OP_VIEW_Y_IROSEKA' in seq and
          'FVP_OPEN_OP_VIEW_Y_HOSHIMEMO' in seq,
          '每作品的 p1..p3 OP y 走 openinganim.h 的宏')
    check('fvp_motion_ease' in seq, '缩放/旋转/位移用 fvp_motion_ease(mode 3)')

    print('== 5. 外壳真的用这套时间轴，且白底后面画标题背景 ==')
    main_c = read(MAIN)
    check('fvp_openseq_layer_state' in main_c,
          'main_win32.c 调 fvp_openseq_layer_state()')
    check('gfx_draw_image_op_xform' in main_c,
          'main_win32.c 用 OP 轴心的 gfx_draw_image_op_xform()')
    check('fvp_openseq_set_elapsed' in main_c,
          'main_win32.c 用墙钟推进（fvp_openseq_set_elapsed）')
    check(re.search(r'title_logo_alpha = 0;\s*\n\s*title_copy_alpha = 0;',
                    main_c) is not None and
          'title_draw_overlay' in main_c,
          '开场期间把标题元素 alpha 压成 0、只画标题背景'
          '（脚本先建标题场景再跑开场）')
    check('fvp_openseq_veil_alpha' not in main_c,
          '旧的白色遮罩（veil）已删除（白底 250 自己就是那个遮罩）')
    check('fvp_openseq_layer_count' in main_c,
          '外壳按层表循环（不写死层数）')

    print('== 6. 开场常量只在一处定义 ==')
    ta = read(TITLEANIM)
    check('FVP_OPENANIM_' not in ta,
          'titleanim.h 不再定义开场常量（已搬到 openinganim.h）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 开场时间轴 == 脚本常量（两个作品），层表/外壳用法一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())