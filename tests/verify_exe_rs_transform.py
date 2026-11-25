#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""RS 分支（缩放/旋转）变换事实的门禁：exe 静态分析 → 基线 → 本工程的换算一致。

冻结内容（`ref/exe-rs-transform.json`，由 `scripts/extract-exe-rs-transform.py`
从 exe 指令字节直接读出）：

1. **角度单位**：渲染角 = 角度单位 × π / 3600 rad = **0.05°/单位**（7200 = 360°）；
   `PrimSetRS`(82) 里 `idiv 3600` 只是取模；矩阵函数是 D3DXMatrixRotationZ(0x446750)。
   之前文档/代码按 0.1°/单位（`/10.0`）算，是错的，本轮修正。
2. **RS 分支变换**：`最终 = T2 + S·(R·(图头 − OP) + prim + accum)`，
   `T2 = (engine+0x60/2, engine+0x64/2)` = 画面中心；矩阵顺序
   R → T1 → S → T2（D3D 行向量）。自洽特例：背景层 s=1 → (−240,−135)（居中裁剪）、
   s=0.8 → (0,0) 整屏铺满。
3. 源码一致性：`src/gfx.c` 四处角度换算必须是 `/ 20.0`；`src/gfx.h` 注释写明
   0.05°/7200；`angle_units` 字段名统一；文档里不再有"0.1°/单位"的旧断言。

缺 exe 时 SKIP 并返回 0。

用法：python tests/verify_exe_rs_transform.py
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
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-rs-transform.py')
REF = os.path.join(FVP, 'ref', 'exe-rs-transform.json')
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
GFX_C = os.path.join(FVP, 'src', 'gfx.c')
GFX_H = os.path.join(FVP, 'src', 'gfx.h')
OPENSEQ_H = os.path.join(FVP, 'src', 'openseq.h')
TITLEBG_H = os.path.join(FVP, 'src', 'titlebg.h')
DOC = os.path.join(FVP, 'docs', 'animation-baseline.md')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    if not os.path.isfile(EXE):
        print('SKIP: 缺 exe %s（可选语料）' % EXE)
        return 0
    for p in (EXTRACT, REF, GFX_C, GFX_H, OPENSEQ_H, TITLEBG_H, DOC):
        if not os.path.isfile(p):
            print('FAIL: 缺 %s' % p)
            return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-exe-rs-transform.py --check 退出码 0')

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 角度单位 ==')
    a = ref['angle']['renderer']
    check(abs(a['divisor'] - 3600.0) < 1e-9, '除数是 3600.0（实际 %r）'
          % a['divisor'])
    check(abs(a['multiplier'] - 3.141592653589793) < 1e-6,
          '乘数是 π（实际 %r）' % a['multiplier'])
    check(abs(a['degrees_per_unit'] - 0.05) < 1e-6,
          '渲染 1 单位 = 0.05°（实际 %.6f）' % a['degrees_per_unit'])
    check(abs(a['units_per_turn'] - 7200.0) < 1.0,
          '每圈 7200 单位（实际 %.1f）' % a['units_per_turn'])
    check(ref['angle']['setter']['modulus'] == 3600,
          'PrimSetRS 取模 3600（脚本层周期）')
    check('D3DXMatrixRotationZ' in a['matrix_fn'],
          '矩阵函数 = D3DXMatrixRotationZ')
    check('0.05' in ref['angle']['ambiguity'],
          '基线写明"渲染 0.05°/单位"的歧义说明')
    check(a['divisor_bytes'] == 'dc3578d74500',
          '除数指令字节冻结（fdiv qword 0x45d778）')
    check(a['multiplier_bytes'] == 'dc0d202e4700',
          '乘数指令字节冻结（fmul qword 0x472e20）')
    check(ref['angle']['setter']['modulus_bytes'] == 'bf100e0000',
          '取模常量指令字节冻结（mov edi,0xe10）')

    print('== 3. RS 分支公式与自洽特例 ==')
    rs = ref['rs_branch']
    check(rs['formula'] == 'final = T2 + S·(R·(img − OP) + prim + accum)',
          '公式冻结')
    check('画面中心' in rs['t2'], 'T2 = 画面中心')
    check([c['name'] for c in rs['matrix_calls']] ==
          ['RotationZ', 'Multiply', 'Translation(T1)', 'Multiply', 'Scaling',
           'Multiply', 'Translation(T2)', 'Multiply'],
          '矩阵调用顺序 R → T1 → S → T2（各步 Multiply）')
    check(any('−240,−135' in x for x in rs['self_checks']),
          '自洽特例：s=1 → (−240,−135) 居中裁剪')
    check(any('0.8' in x for x in rs['self_checks']),
          '自洽特例：s=0.8 → (0,0) 整屏')
    check('not_used_yet' in rs and 'prim.xy' in rs['not_used_yet'],
          '写明 prim.xy≠0 的偏移"还没用上"，不假装已知')

    print('== 4. 源码换算一致（/20.0，不是 /10.0）==')
    gfx_c = io.open(GFX_C, encoding='utf-8').read()
    checks = [
        'rotate_rgba(img, tw, th, -(double)angle_eff / 20.0',
        'xf.angle += (double)p->angle / 20.0',
        '(double)angle_units / 20.0',
        'rotate_rgba(img, w, h, -(double)angle_units / 20.0',
    ]
    for c in checks:
        check(c in gfx_c, 'gfx.c 含 %s' % c)
    bad = [l for l in gfx_c.splitlines()
           if ('angle' in l and '/ 10.0' in l)]
    check(not bad, 'gfx.c 里没有角度换算还用 /10.0（残留 %r）' % bad[:2])
    gfx_h = io.open(GFX_H, encoding='utf-8').read()
    check('0.05' in gfx_h and '7200' in gfx_h,
          'gfx.h 注释写明 0.05°/7200')
    check('angle_units' in gfx_h and 'angle_x10' not in gfx_h,
          'gfx.h 字段名统一为 angle_units')
    for p in (OPENSEQ_H, TITLEBG_H):
        s = io.open(p, encoding='utf-8').read()
        check('angle_units' in s and 'angle_x10' not in s,
              '%s 用 angle_units 命名' % os.path.basename(p))

    print('== 5. 文档不再留"0.1°/单位"的旧断言 ==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('0.05' in doc and '7200' in doc, 'animation-baseline 记录 0.05°/7200')
    check('exe-rs-transform' in doc, 'animation-baseline 指向基线文件')
    stale = []
    for p in ('docs/render-model.md', 'docs/title-menu.md',
              'docs/vm-commands.md'):
        s = io.open(os.path.join(FVP, p), encoding='utf-8').read()
        for line in s.splitlines():
            if '角度' in line and '0.1°' in line:
                stale.append('%s: %s' % (p, line.strip()[:100]))
    check(not stale, '文档里没有"角度 0.1°"旧说法（残留 %r）' % stale[:3])

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 旋转角 = 单位×π/3600（0.05°/单位、7200 一圈），源码 /20.0、'
          '文档与基线一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())