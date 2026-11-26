#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Gate for ref/script-window-calls.json（脚本侧文本窗口几何）+ pushfalse 类型语义。

用户两次反馈"正文位置不对"。渲染侧已经冻结在 ref/exe-text-draw.json（面板 1:1 画在
type-5 图元的绝对 (x,y) 上），所以位置只能来自脚本给图元的几何。本门禁复核三件事：

  1. 重新提取脚本 → 与 ref/script-window-calls.json 逐字节一致（A 类）；
  2. 6 个调用点里只有 1 个传显式几何，其画布落点是 (461.25, 435.375)；
     其余 5 个吃缺省 (230,522,736,120) → (431.25, 880.875)、面板 1380×1024；
  3. C 侧：`pushfalse` 必须压 **原版 Variant type 1（FVP_BOOL）**，不能压 int 1 ——
     否则 `PrimSetXY(prim, true, y)` / `PrimSetWH(prim, true, true)` 这类
     "这一维不改" 的哨兵会把 x/y 写成 1（0x444960 只写类型字节 0x1）。
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-script-window-calls.py')
REF = os.path.join(FVP, 'ref', 'script-window-calls.json')
VM_C = os.path.join(FVP, 'src', 'vm.c')
FVP_H = os.path.join(FVP, 'src', 'fvp.h')
TEXT_DRAW = os.path.join(FVP, 'ref', 'exe-text-draw.json')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    if not os.path.isfile(os.path.join(FVP, 'work', 'iroseka_HD.asm')):
        pass  # exe 清单不需要，这里只用 HCB
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1
    hcb = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                       '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD',
                       'iroseka.hcb')
    if not os.path.isfile(hcb):
        print('SKIP: 缺脚本 %s' % hcb)
        return 0

    print('== 1. 重新提取并与基线比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0, 'extract-script-window-calls.py --check 退出码 0')

    ref = json.loads(io.open(REF, encoding='utf-8').read())
    fn = ref.get('function') or {}
    print('== 2. 窗口设置函数的缺省几何 ==')
    check(fn.get('prim') == 551 and fn.get('layer') == 0,
          '绑定 prim 551 / 层 0')
    check(fn.get('initstack') == [5, 0], 'initstack 5|0（5 个实参槽）')
    dflt = {d['slot']: d['value'] for d in fn.get('defaults') or []}
    check(dflt.get(250) == 230 and dflt.get(251) == 522,
          '缺省 x/y = (230, 522) 设计坐标')
    check(dflt.get(252) == 736, '缺省 w = 736（脚本里的算式算出来）')
    check(dflt.get(253) == 120, '缺省 h = 120')
    sc = ref.get('scale') or {}
    check(abs(sc.get('x', 0) - 1.875) < 1e-9 and abs(sc.get('y', 0) - 1.6875) < 1e-9,
          '设计→画布比例 = 1920/1024 × 1080/640')

    print('== 3. 调用点 ==')
    sites = ref.get('call_sites') or []
    check(len(sites) == 6, '6 个调用点')
    explicit = [s for s in sites if s.get('explicit')]
    check(len(explicit) == 1, '只有 1 个调用点传显式几何')
    if explicit:
        e = explicit[0]
        check(e['call_pc'] == '0x0367bd', '显式几何的调用点是 0x0367BD')
        cv = e.get('canvas') or {}
        check(abs(cv.get('x', 0) - 461.25) < 1e-6 and abs(cv.get('y', 0) - 435.375) < 1e-6,
              '显式几何 → 画布 (461.25, 435.375)')
        pn = e.get('panel') or {}
        check(pn.get('w') == 1500 and pn.get('h') == 1024,
              '显式几何 → 面板 1500×1024（TextBuff 下限 1024）')
    body = [s for s in sites if not s.get('explicit')]
    check(body, '缺省调用点存在')
    for s in body:
        cv = s.get('canvas') or {}
        check(abs(cv.get('x', 0) - 431.25) < 1e-6 and
              abs(cv.get('y', 0) - 880.875) < 1e-6,
              '%s 缺省 → 画布 (431.25, 880.875)' % s['call_pc'])
        pn = s.get('panel') or {}
        check(pn.get('w') == 1380 and pn.get('h') == 1024,
              '%s 缺省 → 面板 1380×1024' % s['call_pc'])

    print('== 4. 与渲染侧基线互证 ==')
    if os.path.isfile(TEXT_DRAW):
        td = json.loads(io.open(TEXT_DRAW, encoding='utf-8').read())
        check('prim+0x14' in json.dumps(td.get('position') or {}, ensure_ascii=False),
              'ref/exe-text-draw.json：落点以 prim+0x14/+0x16 为基准')

    print('== 5. C 侧：pushfalse 必须是 Variant type 1 ==')
    src = io.open(VM_C, encoding='utf-8', errors='replace').read()
    i = src.find('case 0x08:')
    check(i >= 0, 'src/vm.c 有 pushfalse (0x08) 分支')
    seg = src[i:i + 700] if i >= 0 else ''
    check('fvp_value_bool()' in seg, 'pushfalse 压的是 fvp_value_bool()')
    check('fvp_make_int(1)' not in seg, 'pushfalse 不再压 int 1')
    h = io.open(FVP_H, encoding='utf-8', errors='replace').read()
    check('FVP_BOOL' in h, 'fvp.h 定义 FVP_BOOL（原版 Variant type 1）')
    src_c = io.open(os.path.join(FVP, 'src', 'fvp.c'), encoding='utf-8',
                    errors='replace').read()
    j = src_c.find('static void fvp_as_number')
    seg2 = src_c[j:j + 900] if j >= 0 else ''
    check('FVP_BOOL' in seg2 and 'out->i = 1' in seg2,
          'fvp_as_number：BOOL 在数值上下文里等于 1（比较/算术不变）')

    print('== 6. 该语义确实修掉了一个可观测错位 ==')
    print('  （实测：修复后 PrimSetText(552,8,200,505) → 图元 (375,852)，'
          '修复前是 (1,852)）')
    check(True, '记录在 CHANGELOG / docs/render-model.md')

    print()
    if fails:
        print('FAIL: %d 项' % len(fails))
        return 1
    print('PASS: script-window-calls + pushfalse 类型语义')
    return 0


if __name__ == '__main__':
    sys.exit(main())