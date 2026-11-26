#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HCB 脚本里抄出**文本窗口设置函数的全部调用点与实参**（A 类：ref/script-window-calls.json）。

为什么需要它：用户两次反馈"正文位置不对"。渲染侧已经从 exe 读清（`ref/exe-text-draw.json`：
文本面板 1:1 画在 type-5 图元的绝对 (x,y) 上），所以位置只可能由**脚本给图元的几何**决定。
本轮把脚本侧那个窗口设置函数（pc `0x39F27`）的**所有调用点**抄出来：

  * 函数 `0x39F27`：`initstack 5|0`，5 个参数（槽 250..254）各自带 `== true` 缺省惯用法；
  * 缺省值直接读脚本里的字面量：x=230、y=522、w=736（写成
    `800 - ((26+2*30+2)*2) + 26*2` 这个算式）、h=120；
  * 函数内部把坐标交给 helper `0x35C10/(89)/(90)/94/(110)`，helper 再乘
    **x×1.875 = 1920/1024**、**y×1.6875 = 1080/640**（`pushfloat 0x3FF00000 / 0x3FD80000`）；
  * 全部 6 个调用点里**只有 1 个**传了显式几何（`0x0367BD`：246,258,800,320），
    其余 5 个都用 `pushfalse` 哨兵吃缺省值。

产物只含结构与常量（不含任何台词），并且给出"设计坐标 → 画布坐标"的换算结果。

用法：
    python scripts/extract-script-window-calls.py            # 打印 + 写 json
    python scripts/extract-script-window-calls.py --check    # 只比对（门禁用）
    python scripts/extract-script-window-calls.py --hcb PATH # 指定脚本
"""
import argparse
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'ref', 'script-window-calls.json')
DEFAULT_HCB = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                           '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD',
                           'iroseka.hcb')

WINDOW_FN = 0x39F27       # 窗口设置函数（正文窗口 551 在这里绑定）
WINDOW_FN_NARGS = 5       # initstack 5|0
SLOT_BASE = 255 - WINDOW_FN_NARGS  # 250
PRIM_AT = 0x39F82         # pushint16 551
LAYER_AT = 0x39F85        # pushint8 0
DEFAULTS = [              # (缺省值的压栈 pc, 槽号, 缺省值或算式说明)
    (0x39F33, SLOT_BASE + 0, 230, 'x 缺省'),
    (0x39F41, SLOT_BASE + 1, 522, 'y 缺省'),
    (0x39F4F, SLOT_BASE + 2, 736, 'w 缺省（脚本里的算式，见 w_expr）'),
    (0x39F6F, SLOT_BASE + 3, 120, 'h 缺省'),
]
W_EXPR = (0x39F4F, 0x39F64)   # 算式：0x39F4F 起，到 copystack 252 之前
SCALE_X_AT = 0x35C24
SCALE_Y_AT = 0x35C3B
CANVAS_W, CANVAS_H = 1920, 1080
DESIGN_W, DESIGN_H = 1024, 640
# TextBuff(110) 的钳位：每个分量下限 1024（0x35DA0 pushint16 1024 + le）
TEXTBUFF_MIN = 1024


def f32(imm):
    return struct.unpack('<f', struct.pack('<I', imm))[0]


def load_code(hcb):
    sys.path.insert(0, os.path.join(ROOT, 'ref'))
    import fvm_ref  # noqa: E402
    with open(hcb, 'rb') as fp:
        return fvm_ref.Code(fp.read(), 'cp932')


def perr(msg):
    print('FAIL: ' + msg)
    raise SystemExit(1)


def arg_of(instr):
    """一条 push 指令 → (类别, 值)。"""
    if instr is None:
        return ('missing', None)
    size, name, args = instr
    if name in ('pushint8', 'pushint16', 'pushint32'):
        return ('int', int(args[0]))
    if name == 'pushfloat':
        return ('float', f32(args[0]))
    if name == 'pushfalse':
        return ('true', None)
    if name == 'pushtrue':
        return ('false', None)
    if name == 'pushstack':
        return ('stack', int(args[0]))
    if name == 'pushtemp':
        return ('temp', None)
    return (name, None)


def prev_index(code):
    pcs = sorted(code.instr)
    return pcs, {pcs[i]: pcs[i - 1] for i in range(1, len(pcs))}


def eval_expr(code, lo, hi):
    """把 lo..hi 之间的直线算式（pushint*/add/sub/mul）求值 + 还原文本。"""
    stack = []
    parts = []
    pcs = sorted(p for p in code.instr if lo <= p < hi)
    for pc in pcs:
        size, name, args = code.instr[pc]
        if name in ('pushint8', 'pushint16', 'pushint32'):
            v = int(args[0])
            stack.append(v)
            parts.append((str(v), 1))       # (文本, 优先级)
        elif name in ('add', 'sub', 'mul') and len(stack) >= 2:
            b = stack.pop()
            a = stack.pop()
            bt, bp = parts.pop()
            at, ap = parts.pop()
            prec = 1 if name == 'mul' else 0
            op = {'add': '+', 'sub': '-', 'mul': '*'}[name]
            if ap < prec:
                at = '(' + at + ')'
            if bp <= prec and name == 'sub':
                bt = '(' + bt + ')'
            elif bp < prec:
                bt = '(' + bt + ')'
            stack.append({'add': a + b, 'sub': a - b, 'mul': a * b}[name])
            parts.append((at + op + bt, prec))
        else:
            raise SystemExit('FAIL: 算式里出现不支持的指令 %r（0x%06x）' % (name, pc))
    if len(stack) != 1:
        raise SystemExit('FAIL: 算式 0x%06x..0x%06x 求值后栈深 %d' % (lo, hi, len(stack)))
    return stack[0], parts[0][0]


def extract(code):
    ins = code.instr.get(SCALE_X_AT)
    if ins is None or ins[1] != 'pushfloat':
        perr('0x%06x 不是 pushfloat' % SCALE_X_AT)
    sx = f32(ins[2][0])
    ins = code.instr.get(SCALE_Y_AT)
    if ins is None or ins[1] != 'pushfloat':
        perr('0x%06x 不是 pushfloat' % SCALE_Y_AT)
    sy = f32(ins[2][0])
    if abs(sx - CANVAS_W / float(DESIGN_W)) > 1e-6:
        perr('x 比例 %g 不是 1920/1024' % sx)
    if abs(sy - CANVAS_H / float(DESIGN_H)) > 1e-6:
        perr('y 比例 %g 不是 1080/640' % sy)

    fn = code.instr.get(WINDOW_FN)
    if fn is None or fn[1] != 'initstack' or int(fn[2][0]) != WINDOW_FN_NARGS:
        perr('0x%06x 不是 initstack %d（实际 %r）' % (WINDOW_FN, WINDOW_FN_NARGS, fn))

    # ---- 缺省值 ----
    w_val, w_txt = eval_expr(code, W_EXPR[0], W_EXPR[1])
    if w_val != 736:
        perr('w 缺省值算出来是 %d，基线是 736（%s）' % (w_val, w_txt))
    defaults = []
    for pc, slot, val, what in DEFAULTS:
        i = code.instr.get(pc)
        if i is None or i[1] not in ('pushint8', 'pushint16', 'pushint32'):
            perr('0x%06x 不是 pushint*（实际 %r）' % (pc, i))
        got = int(i[2][0])
        expect = w_val if slot == SLOT_BASE + 2 else val
        if got != (800 if slot == SLOT_BASE + 2 else expect):
            perr('0x%06x 压的是 %d，基线是 %d' % (pc, got, expect))
        rec = {'pc': '0x%06x' % pc, 'slot': slot, 'value': expect, 'note': what}
        if slot == SLOT_BASE + 2:
            rec['expr_pc'] = ['0x%06x' % W_EXPR[0], '0x%06x' % (W_EXPR[1] - 1)]
            rec['expr'] = w_txt
            rec['expr_value'] = w_val
            rec['value'] = w_val
        defaults.append(rec)

    prim_i = code.instr.get(PRIM_AT)
    layer_i = code.instr.get(LAYER_AT)
    if prim_i is None or prim_i[1] != 'pushint16' or int(prim_i[2][0]) != 551:
        perr('0x%06x 不是 pushint16 551（实际 %r）' % (PRIM_AT, prim_i))
    if layer_i is None or layer_i[1] != 'pushint8' or int(layer_i[2][0]) != 0:
        perr('0x%06x 不是 pushint8 0（实际 %r）' % (LAYER_AT, layer_i))

    # ---- 调用点 ----
    pcs, prev = prev_index(code)
    calls = [pc for pc in pcs
             if code.instr[pc][1] == 'call' and int(code.instr[pc][2][0]) == WINDOW_FN]
    sites = []
    for pc in calls:
        args = []
        p = pc
        for _ in range(WINDOW_FN_NARGS):
            p = prev.get(p)
            if p is None:
                perr('0x%06x 前面压的实参不够' % pc)
            args.append(arg_of(code.instr[p]))
        args.reverse()          # 压栈顺序 → 参数顺序（槽 250..254）
        # 解析实际生效的几何（哨兵 → 缺省值）
        geo = []
        for k, (kind, val) in enumerate(args):
            if kind == 'int':
                geo.append(val)
            elif kind in ('true', 'false'):
                if k < len(defaults):
                    d = defaults[k]['value'] if kind == 'true' else None
                else:
                    d = None          # 槽 254（第 5 个参数）没有缺省值
                geo.append(d)
            else:
                geo.append(None)
        rec = {
            'call_pc': '0x%06x' % pc,
            'args': [{'kind': k, 'value': v} for k, v in args],
            'design': geo,
            'explicit': all(k == 'int' for k, _ in args[:4]),
        }
        if all(v is not None for v in geo[:4]):
            x, y, w, h = geo[:4]
            rec['canvas'] = {
                'x': round(x * sx, 3), 'y': round(y * sy, 3),
                'w': round(w * sx, 3), 'h': round(h * sy, 3),
            }
            rec['panel'] = {
                'w': max(int(round(w * sx)), TEXTBUFF_MIN),
                'h': max(int(round(h * sy)), TEXTBUFF_MIN),
            }
        sites.append(rec)

    return {
        'note': 'HCB 脚本里文本窗口设置函数（0x39F27）的全部调用点与实参（A 类事实）；'
                '由 scripts/extract-script-window-calls.py 生成',
        'function': {
            'pc': '0x%06x' % WINDOW_FN,
            'initstack': [WINDOW_FN_NARGS, 0],
            'slots': [SLOT_BASE, SLOT_BASE + WINDOW_FN_NARGS - 1],
            'prim': 551,
            'layer': 0,
            'defaults': defaults,
            'sentinel': 'pushfalse = 「用缺省值」',
        },
        'scale': {'x': sx, 'y': sy,
                  'design': [DESIGN_W, DESIGN_H],
                  'canvas': [CANVAS_W, CANVAS_H]},
        'textbuff_min': TEXTBUFF_MIN,
        'call_sites': sites,
        'unknown': [
            '5 个缺省调用点分别在什么场景触发（要运行期才能确定执行顺序）',
            '参数 5（槽 254）的用途：0x39F73 之后与 pushglobal 51/249 组合使用',
        ],
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--hcb', default=DEFAULT_HCB)
    opt = ap.parse_args(argv)
    if not os.path.isfile(opt.hcb):
        print('SKIP: 缺脚本 %s' % opt.hcb)
        return 0
    code = load_code(opt.hcb)
    result = extract(code)
    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            perr('缺基线 %s' % OUT_JSON)
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            perr('重新提取与 ref/script-window-calls.json 不一致')
        print('OK: ref/script-window-calls.json 与脚本一致（%d 个调用点）'
              % len(result['call_sites']))
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('== 窗口设置函数 0x%06x 的调用点：%d 个 ==' % (WINDOW_FN, len(result['call_sites'])))
    for s in result['call_sites']:
        cv = s.get('canvas') or {}
        print('  %s  design=%s  canvas=(%s,%s) panel=%s%s'
              % (s['call_pc'], s['design'], cv.get('x'), cv.get('y'),
                 s.get('panel'), '' if s['explicit'] else '  [缺省]'))
    return 0


if __name__ == '__main__':
    sys.exit(main())