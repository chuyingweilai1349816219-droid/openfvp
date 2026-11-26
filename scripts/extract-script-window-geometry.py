#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 HCB 脚本里抄出**文本窗口几何**（A 类：设计坐标 → 画布坐标的换算 + 四个窗口）。

为什么需要它：以前文档里写"正文窗口在 prim (1,1)、面板 1024×1024、每行水平居中
+ 顶端对齐"，那个 (1,1) 其实来自**我们自己 VM 的一次运行**，不是脚本数据。
本轮把脚本侧的窗口设置链路读出来，结论完全不同：

  * 脚本里的坐标是**设计空间 1024×640**，所有窗口/图元 helper 先把坐标乘上
    **1.875 (x) = 1920/1024**、**1.6875 (y) = 1080/640** 再调引擎命令；
  * 四个文本窗口的绑定（id / 层号 / 设计坐标）都是脚本里的**字面量**（正文那
    一处用函数局部变量的默认值，也在脚本里写着）。

抄法（只抄，不猜）：脚本指令流按 pc 取指令；helper 的换算常量直接从
`pushfloat <imm32>` 的立即数按 float32 解出来（0x3FF00000 → 1.875、
0x3FD80000 → 1.6875），绑定点的实参直接读 `pushint*` 立即数。

产物 `ref/script-window-geometry.json`（只含结构与常量，不含任何台词）。

用法：
    python scripts/extract-script-window-geometry.py            # 打印 + 写 json
    python scripts/extract-script-window-geometry.py --check    # 只比对（门禁用）
    python scripts/extract-script-window-geometry.py --hcb PATH # 指定脚本
"""
import argparse
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'ref', 'script-window-geometry.json')
DEFAULT_HCB = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                           '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD',
                           'iroseka.hcb')

# helper：pc → (命令号, 说明)。换算常量与 syscall 号都在门禁里逐条复核。
HELPERS = [
    (0x35C10, 86, 'PrimSetText(prim, layer, x*1.875, y*1.6875)'),
    (0x35CF2, 89, 'PrimSetWH(prim, w*1.875, h*1.6875)'),
    (0x35D14, 90, 'PrimSetXY(prim, x*1.875, y*1.6875)（true 哨兵 = 不改）'),
    (0x35D56, 94, 'syscall 94(a, w*1.875, h*1.6875)'),
    (0x35D76, 110, 'TextBuff(layer, w*1.875, h*1.6875)，宽高钳到 1024'),
]

# 窗口绑定：prim / 层号 / 设计坐标（x, y 可能是函数局部变量的默认值）
BINDINGS = [
    (0x0364F1, 'name', 552, 8),
    (0x039F82, 'body', 551, 0),
    (0x03A046, 'overlay', 558, 10),
    (0x03AA7B, 'chapter', 553, 9),
]

# 帧模型证据：窗口 helper 与窗口设置函数的 `initstack A|B` 及其槽号用法
FRAME_FUNCS = [
    (0x35C10, 'prim_set_text', 'PrimSetText wrapper'),
    (0x35D14, 'prim_set_xy', 'PrimSetXY wrapper'),
    (0x35D76, 'text_buff', 'TextBuff wrapper'),
    (0x39F27, 'window_setup', '窗口设置（正文窗口 551 的绑定在这里）'),
    # 两个高频函数用来复核"参数槽 = 255-A..254"：都用同一个 `== true` 缺省惯用法
    (0x23804, 'probe_2args', 'initstack 2|0：参数槽 253/254（写 254）'),
    (0x3F389, 'probe_8args', 'initstack 8|0：参数 1 是槽 251（写 251）'),
]
WINDOW_SETUP_CALL = 0x363C5   # 调用 0x39F27 之前连压 5 个 pushtrue 哨兵

SCALE_X_AT = 0x35C24   # pushfloat <imm32>（x 的比例）
SCALE_Y_AT = 0x35C3B   # pushfloat <imm32>（y 的比例）
CLAMP_AT = 0x35DA0     # pushint16 1024（TextBuff 的钳位）


def f32(imm):
    return struct.unpack('<f', struct.pack('<I', imm))[0]


def arg_text(instr):
    """把一条 push 指令解成 (值, 文本)。非字面量返回 (None, 'runtime')。"""
    op, name, args = instr
    if name in ('pushint8', 'pushint16', 'pushint32'):
        return int(args[0]), str(int(args[0]))
    if name == 'pushfloat':
        return None, 'float(%g)' % f32(args[0])
    if name == 'pushstack':
        return None, 'stack%d' % args[0]
    if name == 'pushtrue':
        return None, 'true'
    if name == 'pushfalse':
        return None, 'false'
    return None, name


def load_code(hcb):
    sys.path.insert(0, os.path.join(ROOT, 'ref'))
    import fvm_ref  # noqa: E402
    with open(hcb, 'rb') as fp:
        return fvm_ref.Code(fp.read(), 'cp932')


def next_pc(code, pc):
    return pc + code.size[pc]


def perr(msg):
    print('FAIL: ' + msg)
    raise SystemExit(1)


def extract(code):
    # ---- 1) 换算常量 ----
    ins = code.instr.get(SCALE_X_AT)
    if ins is None or ins[1] != 'pushfloat':
        perr('0x%06x 不是 pushfloat（实际 %r）' % (SCALE_X_AT, ins))
    sx = f32(ins[2][0])
    ins = code.instr.get(SCALE_Y_AT)
    if ins is None or ins[1] != 'pushfloat':
        perr('0x%06x 不是 pushfloat（实际 %r）' % (SCALE_Y_AT, ins))
    sy = f32(ins[2][0])
    ins = code.instr.get(CLAMP_AT)
    if ins is None or ins[1] != 'pushint16' or ins[2][0] != 1024:
        perr('0x%06x 不是 pushint16 1024（实际 %r）' % (CLAMP_AT, ins))

    # ---- 2) helper 的 syscall 号 ----
    helpers = []
    for pc, cid, what in HELPERS:
        limit = pc + 96
        found = []
        p = pc
        while p <= limit:
            i = code.instr.get(p)
            if i is None:
                break
            if i[1] == 'ret':
                break
            if i[1] == 'syscall':
                found.append(i[2][0])
            p = next_pc(code, p)
        if cid not in found:
            perr('helper 0x%06x 的 syscall 序列 %r 里没有 %d'
                 % (pc, found, cid))
        helpers.append({'pc': '0x%06x' % pc, 'syscall': cid, 'what': what})

    # ---- 3) 窗口绑定 ----
    bindings = []
    for pc, role, prim, layer in BINDINGS:
        i = code.instr.get(pc)
        if i is None or i[1] not in ('pushint16', 'pushint8'):
            perr('0x%06x 不是 pushint*（实际 %r）' % (pc, i))
        got_prim = i[2][0]
        p = next_pc(code, pc)
        i2 = code.instr.get(p)
        got_layer = i2[2][0] if i2 is not None else None
        p = next_pc(code, p)
        xval, xtxt = arg_text(code.instr.get(p))
        p = next_pc(code, p)
        yval, ytxt = arg_text(code.instr.get(p))
        # 绑定的 y 可能写成 `344 + 4 + 2` 这种 add 链：继续吃 `pushint*; add`
        p = next_pc(code, p)
        while True:
            a = code.instr.get(p)
            b = code.instr.get(next_pc(code, p)) if a is not None else None
            if (a is None or b is None or a[1] not in
                    ('pushint8', 'pushint16', 'pushint32') or b[1] != 'add'):
                break
            if yval is not None:
                yval += a[2][0]
                ytxt = '%d' % yval
            p = next_pc(code, next_pc(code, p))
        if got_prim != prim or got_layer != layer:
            perr('0x%06x 实参 (%d,%d) 与期望 (%d,%d) 不符'
                 % (pc, got_prim, got_layer, prim, layer))
        rec = {'pc': '0x%06x' % pc, 'role': role,
               'prim': got_prim, 'layer': got_layer,
               'design': {'x': xtxt, 'y': ytxt}}
        if xval is not None and yval is not None:
            rec['canvas'] = {'x': int(xval * sx), 'y': int(yval * sy)}
        bindings.append(rec)

    # ---- 4) 正文窗口的默认值（局部变量 250/251 的默认赋值）----
    body_defaults = []
    for want, val in ((250, 230), (251, 522)):
        hit = None
        for pc in sorted(code.instr):
            if not (0x039F20 <= pc <= 0x039F70):
                continue
            i = code.instr[pc]
            if i[1] == 'pushint16' and i[2][0] == val:
                p = next_pc(code, pc)
                j = code.instr.get(p)
                if j is not None and j[1] == 'copystack' and j[2][0] == want:
                    hit = pc
                    break
        if hit is None:
            perr('没找到 copystack %d 的默认值 %d' % (want, val))
        body_defaults.append({'local': want, 'value': val,
                              'pc': '0x%06x' % hit})

    # ---- 5) 帧模型证据（initstack A|B + 槽号用法）----
    frame_model = []
    for fpc, role, what in FRAME_FUNCS:
        i = code.instr.get(fpc)
        if i is None or i[1] != 'initstack':
            perr('0x%06x 不是 initstack（实际 %r）' % (fpc, i))
        a_decl, b_decl = i[2][0], i[2][1]
        slots = []
        p = next_pc(code, fpc)
        guard = 0
        while guard < 600:
            j = code.instr.get(p)
            if j is None:
                break
            if j[1] == 'initstack':   # 下一个函数开始
                break
            if j[1] in ('pushstack', 'copystack') and j[2][0] >= 0xFA:
                slots.append(j[2][0])
            p = next_pc(code, p)
            guard += 1
        frame_model.append({
            'pc': '0x%06x' % fpc, 'role': role, 'what': what,
            'initstack': [a_decl, b_decl],
            'slot_min': min(slots) if slots else None,
            'slot_max': max(slots) if slots else None,
            'slot_reads': len(slots),
        })

    # 窗口设置函数的调用点：连压 5 个 pushtrue（显式"用默认值"哨兵）
    sentinels = []
    p = WINDOW_SETUP_CALL
    for _ in range(5):
        j = code.instr.get(p)
        if j is None or j[1] != 'pushtrue':
            perr('0x%06x 不是 pushtrue（实际 %r）' % (p, j))
        sentinels.append('0x%06x' % p)
        p = next_pc(code, p)
    j = code.instr.get(p)
    if j is None or j[1] != 'call' or j[2][0] != 0x39F27:
        perr('0x%06x 不是 call 0x39F27（实际 %r）' % (p, j))

    return {
        'frame_model': {
            'note': '`initstack A|B`：A = 参数槽个数、B = 额外局部变量个数。参数占绝对槽 '
                    '255-A..254（结束于槽 254），额外局部变量占它们下面紧挨的 B 个槽，'
                    '也可以用 0..B-1 寻址。',
            'functions': frame_model,
            'window_setup_call': {
                'pc': '0x%06x' % p,
                'pushtrue_sentinels': sentinels,
                'what': '调用方在 call 0x39F27 之前连压 5 个 `pushtrue`，'
                        '函数体用 `if (槽 == true) 槽 = 默认值` 吃默认值——'
                        '说明 `true` 是**显式**哨兵，不是"没传"的填充值',
            },
            'known_gap': {
                'what': '本引擎 C VM 的旧帧模型把 `copystack N` 限制在 N < initstack 第一个字节，'
                        '所以脚本对槽 250..254 的默认值写入全部被丢掉：'
                        '`PrimSetText(551, 0, …)` 实测拿到 (1, 1)，而脚本要的是 (230, 522)'
                        '（换算后画布 (431, 880)）。',
                'expected_canvas': {'x': int(230 * sx), 'y': int(522 * sy)},
                'engine_logs': {'x': 1, 'y': 1},
                'why_not_fixed_yet': '把槽寻址按 255-A..254 打开后，'
                                     '`tests/compare.py` 的冻结文本基线（ref/ref-900k.txt）'
                                     '出现差异（6496 → 6760 B，第 3 行多一个字符）：'
                                     '说明那份基线本身是用旧帧模型生成的。'
                                     '要修就得同时给出"新输出更接近原版"的证据再重新冻结基线，'
                                     '不能悄悄改。',
            },
        },
        'note': '从 HCB 脚本静态分析抄出的文本窗口几何（A 类）：脚本坐标是设计空间，'
                '窗口 helper 先把 x 乘 1.875、y 乘 1.6875 再调引擎命令；由 '
                'scripts/extract-script-window-geometry.py 生成',
        'script_coords': {
            'design': [1024, 640],
            'canvas': [1920, 1080],
            'scale_x': sx,
            'scale_y': sy,
            'scale_x_from': '1920/1024',
            'scale_y_from': '1080/640',
            'float_bits': {'x': '0x%08x' % code.instr[SCALE_X_AT][2][0],
                           'y': '0x%08x' % code.instr[SCALE_Y_AT][2][0]},
            'note': '换算发生在脚本里（helper），引擎收到的已经是画布坐标；'
                    '引擎侧**不要**再缩一次',
        },
        'textbuff_clamp': {
            'pc': '0x%06x' % CLAMP_AT,
            'width': 1024,
            'height': 1024,
            'note': 'TextBuff helper 把换算后的宽高各自钳到 1024 再调 TextBuff；'
                    '所以全屏面板的实测值就是 1024×1024，而不是设计尺寸',
        },
        'helpers': helpers,
        'windows': bindings,
        'body_window_defaults': body_defaults,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--hcb', default=DEFAULT_HCB)
    opt = ap.parse_args(argv)

    if not os.path.isfile(opt.hcb):
        print('SKIP: 缺 HCB 脚本 %s（游戏数据不入库）' % opt.hcb)
        return 0

    code = load_code(opt.hcb)
    result = extract(code)
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'

    sc = result['script_coords']
    print('== 设计坐标 → 画布坐标 ==')
    print('  设计 %dx%d → 画布 %dx%d；x*%.4f（%s）、y*%.4f（%s）'
          % (sc['design'][0], sc['design'][1], sc['canvas'][0], sc['canvas'][1],
             sc['scale_x'], sc['scale_x_from'], sc['scale_y'], sc['scale_y_from']))
    print('  float 立即数：x=%s y=%s' % (sc['float_bits']['x'],
                                         sc['float_bits']['y']))
    print('== 窗口 helper（%d 个） ==' % len(result['helpers']))
    for h in result['helpers']:
        print('  %s syscall %-3d %s' % (h['pc'], h['syscall'], h['what']))
    print('== 文本窗口绑定（%d 个） ==' % len(result['windows']))
    for w in result['windows']:
        cv = w.get('canvas')
        print('  %-8s prim %-4d layer %-3d 设计 (%s, %s)%s'
              % (w['role'], w['prim'], w['layer'],
                 w['design']['x'], w['design']['y'],
                 '' if cv is None else '  → 画布 (%d, %d)' % (cv['x'], cv['y'])))
    print('== 帧模型（initstack A|B 与槽号）==')
    fm = result['frame_model']
    for f in fm['functions']:
        print('  %-16s %s  initstack %s  槽 %s..%s（%d 次读写）'
              % (f['role'], f['pc'], f['initstack'], f['slot_min'],
                 f['slot_max'], f['slot_reads']))
    print('  窗口设置调用点 %s 前 5 条都是 pushtrue 哨兵'
          % fm['window_setup_call']['pc'])
    print('== 正文窗口默认值 ==')
    for d in result['body_window_defaults']:
        print('  局部变量 %d 默认 %d（%s）' % (d['local'], d['value'], d['pc']))

    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())