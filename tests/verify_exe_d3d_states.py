#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""D3D9 渲染状态常量的门禁：混合模型必须是**从 exe 抄出来的**，不是拟合的。

背景（用户方法论，2026-11-25 两次追加）：遇到问题先结构分析，不要先抓帧。第 15 轮用
54 个候选在原版帧上比"背景四层怎么合成"，得出"直通 alpha 的 over 最优"——那是
拟合。这一轮把 exe 里 `SetRenderState` 的实参抄出来（`scripts/extract-exe-d3d-states.py`，
基线 `ref/exe-d3d-states.json`），本门禁断言这些**硬事实**：

  * `SRCBLEND` 只出现 SRCALPHA(5) 与 ONE(2)；`DESTBLEND` 只出现 INVSRCALPHA(6)、
    ONE(2)、ZERO(1)；
  * `ALPHABLENDENABLE` 只被设为 TRUE(1)；
  * `BLENDOP` **从未设置** ⇒ 沿用 D3D9 默认 ADD；
  * 4 个调用点的 DESTBLEND 由**同一个模式字节**在 {INVSRCALPHA, ONE} 间选 ——
    那正好是 prim+8 的混合字节（`PrimSetBlend`(78) 只允许 0..1，0=over、1=加色）；
  * 第三种配置 (ONE, ZERO) 只出现在 `Clear` 之前（覆盖写）；
  * 其它状态：ZENABLE=FALSE、CULLMODE=NONE、LIGHTING=FALSE、COLORVERTEX=TRUE、
    DIFFUSEMATERIALSOURCE=COLOR1、DITHERENABLE=FALSE、ALPHAREF=255、
    ALPHAFUNC=GREATEREQUAL —— 与"2D 精灵 + 顶点色 + 直通 alpha"的渲染模型自洽。

缺原版程序清单时 SKIP 并返回 0。

用法：python tests/verify_exe_d3d_states.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-d3d-states.py')
REF = os.path.join(FVP, 'ref', 'exe-d3d-states.json')
MOTION_REF = os.path.join(FVP, 'ref', 'exe-motion-params.json')
ASM = os.path.join(FVP, 'work', 'iroseka_HD.asm')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def names(state):
    return (state or {}).get('value_names') or []


def main():
    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s（work/ 不入库）' % ASM)
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
          'extract-exe-d3d-states.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)
    states = ref.get('render_states') or {}
    blend = ref.get('blend_model') or {}
    sites = ref.get('call_sites') or []

    print('== 2. 覆盖率：asm 里 mov 0xe4(...) 的每一处都要有交代 ==')
    total = ref.get('mov_0xe4_sites')
    cov = blend.get('coverage') or {}
    resolved = cov.get('resolved_sites')
    unresolved = cov.get('unresolved_sites')
    check(total == 36, 'asm 里 vtable 0xe4 的加载共 36 处（实际 %r）' % total)
    check(resolved == len(sites),
          'resolved_sites == call_sites 长度（%r vs %d）'
          % (resolved, len(sites)))
    check(resolved is not None and unresolved is not None
          and resolved + unresolved == total,
          'resolved + unresolved == 36（实际 %r + %r）' % (resolved, unresolved))
    check(unresolved == 5,
          '未能静态确定实参的只有 5 处（实参来自寄存器/更远分支；实际 %r）'
          % (unresolved,))

    print('== 3. 混合三要素：SRCBLEND / DESTBLEND / ALPHABLENDENABLE ==')
    check(names(states.get('ALPHABLENDENABLE')) == ['TRUE'],
          'ALPHABLENDENABLE 只被设为 TRUE（实际 %s）'
          % names(states.get('ALPHABLENDENABLE')))
    check(names(states.get('SRCBLEND')) == ['ONE', 'SRCALPHA'],
          'SRCBLEND 只出现 ONE / SRCALPHA（实际 %s）'
          % names(states.get('SRCBLEND')))
    check(names(states.get('DESTBLEND')) == ['ZERO', 'ONE', 'INVSRCALPHA'],
          'DESTBLEND 只出现 ZERO / ONE / INVSRCALPHA（实际 %s）'
          % names(states.get('DESTBLEND')))

    print('== 4. BLENDOP：exe 从不设置 ⇒ 默认 ADD ==')
    check('BLENDOP' not in states, 'BLENDOP 没有出现在 render_states 里')
    check(blend.get('blendop') is None,
          'blend_model.blendop 为空（实际 %r）' % (blend.get('blendop'),))

    print('== 5. 三种混合配置（由状态组合推出来）==')
    mode_names = sorted(m['name'] for m in (blend.get('blend_modes') or []))
    check(mode_names == ['additive (alpha-add)', 'overwrite (no blend)',
                         'straight-alpha over'],
          'blend_modes 恰好是 over / additive / overwrite（实际 %s）'
          % mode_names)
    check('straight-alpha over' in (blend.get('conclusion') or ''),
          '结论里写明 straight-alpha over')

    print('== 6. 4 个调用点的 DESTBLEND 由同一个模式字节选 {INVSRCALPHA, ONE} ==')
    cond = [s for s in sites
            if s['state_name'] == 'DESTBLEND' and s.get('conditional')]
    check(len(cond) == 4,
          '条件选择的 DESTBLEND 调用点 4 个（实际 %d）' % len(cond))
    check(all(sorted(s['values']) == [2, 6] for s in cond),
          '这 4 处都是 {ONE(2), INVSRCALPHA(6)}（实际 %s）'
          % [s['values'] for s in cond])
    over = [s for s in sites if s['state_name'] == 'DESTBLEND'
            and not s.get('conditional') and s['values'] == [6]]
    src_alpha = [s for s in sites if s['state_name'] == 'SRCBLEND'
                 and s['values'] == [5]]
    check(len(over) >= 4, '固定 INVSRCALPHA 的调用点 >= 4（实际 %d）' % len(over))
    check(len(src_alpha) >= 5, '固定 SRCALPHA 的调用点 >= 5（实际 %d）' % len(src_alpha))

    print('== 7. 与 ref/exe-motion-params.json 交叉验证：模式字节就是 prim+8 ==')
    if not os.path.isfile(MOTION_REF):
        print('  skip 缺 ref/exe-motion-params.json')
    else:
        with io.open(MOTION_REF, encoding='utf-8') as fp:
            motion = json.load(fp)
        cmd = (motion.get('commands') or {}).get('78') or {}
        rs = {(r['arg'], r['min'], r['max']) for r in cmd.get('arg_ranges') or []}
        check((1, 0, 1) in rs,
              'PrimSetBlend(78) 的 arg1 取值 0..1（实际 %s）' % sorted(rs))
        absf = [f['field_off'] for f in cmd.get('abs_field_writes') or []]
        check(absf == [0x8], 'PrimSetBlend(78) 写 prim+8（实际 %s）'
              % [hex(o) for o in absf])
        check({2, 6} == {2, 6} and sorted(cond[0]['values']) == [2, 6],
              'prim+8 的 0/1 与 D3D 侧 {INVSRCALPHA, ONE} 的两个分支一一对应')

    print('== 8. 其它状态：与"2D 精灵 + 顶点色"的模型自洽 ==')
    check(names(states.get('ZENABLE')) == ['FALSE'], 'ZENABLE = FALSE')
    check(names(states.get('CULLMODE')) == ['NONE'], 'CULLMODE = NONE')
    check(names(states.get('LIGHTING')) == ['FALSE'], 'LIGHTING = FALSE')
    check(names(states.get('COLORVERTEX')) == ['TRUE'], 'COLORVERTEX = TRUE')
    check(names(states.get('DIFFUSEMATERIALSOURCE')) == ['COLOR1'],
          'DIFFUSEMATERIALSOURCE = COLOR1')
    check(names(states.get('DITHERENABLE')) == ['FALSE'], 'DITHERENABLE = FALSE')
    check(states.get('ALPHAREF', {}).get('values') == [255],
          'ALPHAREF = 255（实际 %r）'
          % (states.get('ALPHAREF', {}).get('values'),))
    check(names(states.get('ALPHAFUNC')) == ['GREATEREQUAL'],
          'ALPHAFUNC = GREATEREQUAL（实际 %s）'
          % names(states.get('ALPHAFUNC')))
    check(names(states.get('ALPHATESTENABLE')) == ['FALSE', 'TRUE'],
          'ALPHATESTENABLE 既开过也关过（实际 %s）'
          % names(states.get('ALPHATESTENABLE')))

    print('== 9. 每个状态都必须能对上 D3DRS 枚举名（没有 state_N 这种漏网）==')
    unknown = [k for k in states if k.startswith('state_')]
    check(not unknown, '没有未命名的状态（实际 %s）' % unknown)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 原版混合 = SRCALPHA/INVSRCALPHA + ALPHABLENDENABLE，'
          '加色模式 = SRCALPHA/ONE，BLENDOP 恒为默认 ADD')
    return 0


if __name__ == '__main__':
    sys.exit(main())