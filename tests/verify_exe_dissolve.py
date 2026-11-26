#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""`Dissolve` / `DissolveWait` 状态机门禁（ref/exe-dissolve.json）。

主张（2026-11-27 重查）：

  1. 带 gfx 的外壳空转在 **`0x59264..0x5928C`** 的循环里（pctrace 实测 953 次/2 万步），
     这个循环唯一的出口是 `DissolveWait(15)` 返回 **false**；
  2. exe 的 `DissolveWait` handler `0x437520`：状态 `engine+0x101010 == 0` 时什么都不做；
     实参 type==0 时只把返回类型写成 1（**不写值**）；非空实参置线程标志 `0x10` 与
     `+0x810 = 1`（等待）。**它从不写返回值** —— 所以循环出口取决于线程 temp 的残留值；
  3. `Dissolve(14)` 是屏幕淡出：状态 3/6，`+0x101018` 总时长、`+0x10101C` 已用时，
     tick `0x436FC0` 推进并在结束时把状态归 0（state==3 归 1）；渲染用
     `alpha = 0xFF - elapsed*255/duration` 写淡出图元 `+0x7`。

这条门禁只钉"事实"，不钉结论：循环为什么在原版能出来，还要继续查变体比较语义
（`--bool-type` 下卡点会换到 `0x235C9`，说明比较语义确实牵动控制流）。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'exe-dissolve.json')
EXTRACTOR = os.path.join(FVP, 'scripts', 'extract-exe-dissolve.py')


def load_extractor():
    spec = importlib.util.spec_from_file_location('extract_exe_dissolve', EXTRACTOR)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not os.path.isfile(a.json):
        print('verify_exe_dissolve: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-dissolve/1', 'schema fvp-dissolve/1')
    sites = ref.get('sites', [])
    check(len(sites) == 14, '14 个 exe 站点（实际 %d）' % len(sites))
    check(all(s.get('ok') for s in sites), '每个站点都标注 ok')
    pcs = [s['pc'] for s in sites]
    check('0x43752c' in pcs and '0x43a06d' in pcs and '0x436ff0' in pcs,
          'DissolveWait / Dissolve / tick 三块都在')

    print('== DissolveWait 不写返回值')
    check(any('movb   $0x1,0x814' in s['expect'] for s in sites),
          'null 实参只写返回类型 1（0x814）')
    check(not any('0x818' in s['expect'] for s in sites),
          '没有任何站点写返回值（0x818）—— 出口取决于残留 temp')
    check(any('orl    $0x10,0x808' in s['expect'] for s in sites) and
          any('movb   $0x1,0x810' in s['expect'] for s in sites),
          '非空实参：线程标志 0x10 + 0x810 = 1（等待）')

    print('== 状态机与淡出')
    st = ref.get('state', {})
    check(st.get('addr') == 'engine+0x101010', '状态在 engine+0x101010')
    check(st.get('idle') == 0 and st.get('phases') == [1, 2, 3, 4, 5, 6],
          '状态 0=空闲、1..6=阶段')
    check(st.get('duration') == 'engine+0x101018' and
          st.get('elapsed') == 'engine+0x10101c', '总时长/已用时字段')
    check(st.get('fade_prim') == 'engine+0x100fdc' and st.get('fade_alpha_offset') == 7,
          '淡出图元与其 alpha 偏移')

    print('== 脚本侧循环')
    loop = ref.get('loop', {})
    check(loop.get('region') == ['0x59264', '0x5928c'], '循环区间 0x59264..0x5928C')
    check(loop.get('exit') == 'DissolveWait(15) == false', '唯一出口')
    script = loop.get('script', [])
    check([s['pc'] for s in script] == ['0x59264', '0x59265', '0x5926b', '0x5928c'],
          '4 个脚本站点齐备')
    check('0x592DF' in loop.get('not_the_265_loop', ''),
          'note 说明 265 的循环在这段之后（不是同一个循环）')

    print('== 重新抽取反汇编对账')
    mod = load_extractor()
    got = mod.build()
    if got is None:
        print('  skip  work/iroseka_HD.asm not present')
    else:
        check(got['sites'] == sites, '站点逐字段相等')
        check(got['state'] == st, '状态机字段相等')
        check(got['loop'] == loop, '循环描述相等')

    if fails:
        print('verify_exe_dissolve: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_exe_dissolve: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())