#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从脚本里静态提取**开场 FAVORITE logo 序列**（不跑原版、不截图）。

背景：`--opening` 以前是一条**按抓帧时间线拟合**的时间轴（白底 300 / 淡入 1200 /
停留 2100 / 淡出 1200 / 白场 600，`src/openseq.h` 旧注释写着"时长与缓动是近似值"）。
按本项目的方法论（2026-11-25 用户两次强调：先结构分析，不要先抓帧），这条时间轴必须从
脚本里读出来。本轮把开场函数整段解出来：

  函数入口：`initstack; ... ; call 0x3F6A0(0,0,0,true); <flag>; popglobal 9`
  （五彩斑斓 0x58096、星空 0x6EFF1；两个作品的**同一段脚本**，只有逻辑画布尺寸不同）

脚本对每个图元做的事（槽位 250/252/254/255/256/257 = logo_bg / logo_favo /
logo_favo_view / logo_favo_view_p1..p3，GraphLoad 的字符串可反查）：

  1. `PrimSetAlpha(prim, 0)` / `PrimSetZ(prim, 1000)` / `PrimSetOP(prim, 逻辑中心)`
     —— OP 用 helper 把逻辑坐标换算成 HD 像素（五彩斑斓 1024x640→1920x1080，
     星空 800x600→1920x1080）；
  2. 淡入：`MotionAlpha(prim, 0, 255, ms)`；`logo_favo` 1200 ms，
     `logo_favo_view` 与 p1..p3 1400 ms；`logo_bg` 不淡入（一开始就是 255）；
  3. 缩放/旋转/位移（都带缓动 mode 3）：
       * `logo_favo`/`logo_favo_view`：`MotionMoveZ(prim, 750, 1000, ms)`（0.75→1.0）；
       * p1..p3：`MotionMoveS2(prim, 10000, 1000, 10000, 1000, 1400, 3)`、
         `MotionMoveR(prim, 3600, 0, 1400, 3)`、
         `MotionMove(prim, 逻辑坐标, 0, 0, 1400, 3)`（从画外/偏移处收拢到原位）；
  4. `MotionAlphaTest(252)` 等淡入结束（1200 ms）后开始淡出：
       * `MotionAlpha(250, keep, 0, 3500)`（白底 3.5 s 淡出）；
       * `MotionAlpha(252/253/254/255/256/257, keep, 0, 2500)`（logo 层 2.5 s 淡出）；
       * 最后 `MotionAlpha(全部, keep, 0, 100)`（100 ms 快速收尾）→ 函数返回。

所以真实时长 = 1200 + 2500 + 100 = **3800 ms**，不是旧模型的 5400 ms。

产物 `ref/opening-scene.json`（只含槽位/时长/坐标/轴心等参数，不含游戏数据本体）。

用法：
    python scripts/extract-opening-scene.py            # 打印 + 写 json
    python scripts/extract-opening-scene.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': {
        'hcb': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
        'dir': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
    },
    'hoshimemo': {
        'hcb': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                            'Hoshimemo_HD.hcb'),
        'dir': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
    },
}
OUT_JSON = os.path.join(ROOT, 'ref', 'opening-scene.json')
OPEN_NAMES = ['logo_bg', 'logo_favo', 'logo_favo_view', 'logo_favo_view_p1',
              'logo_favo_view_p2', 'logo_favo_view_p3']
OPEN_PRIMS = (250, 257)

# 可以在实参块里出现的"纯"指令（不改变控制流、不读内存）。
PURE = {
    'pushint8', 'pushint16', 'pushint32', 'pushfloat', 'pushstring',
    'pushfalse', 'pushtrue',
    'add', 'sub', 'mul', 'div', 'mod', 'neg',
    'eq', 'neq', 'lt', 'gt', 'le', 'ge', 'logand', 'logor', 'not', 'test',
    'bitand', 'bitor', 'bitxor', 'shl', 'shr',
}
# 本轮关心的命令：Motion* 与 PrimSet*（名字来自 ref/cmd_table.json）。
MOTION_CMDS = {42, 44, 48, 49, 51, 52, 54, 55, 57, 58}
PRIM_CMDS = {77, 78, 81, 82, 89, 90, 91}
# helper 里被包装的命令 → 需要做"逻辑坐标→HD 像素"换算的实参下标与个数。
#   MotionMove(48): (prim, from_x, from_y, to_x, to_y, ms, mode, flag)
#   PrimSetOP(81):  (prim, op_x, op_y)
COORD_ARGS = {48: (1, 4), 81: (1, 2)}


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def f32round(v):
    """按原版 VM 的精度取整到 float32。

    原版 Variant 的 float 是 **float32**（`docs/vm-commands.md` §7：pushfloat 的
    handler 0x445B10 用 `flds`/`fstps`，type=3），所以脚本里的 `mul` 也是 float32
    算术。星空序里 `300 * 1.8f` 在 double 下得 539.99998（截断 539），在 float32
    下正好舍入回 540.0 —— 必须按 float32 算才和原版一致。
    """
    return struct.unpack('<f', struct.pack('<f', float(v)))[0]


def c_trunc(v):
    """C 的 (int) 截断：向零取整。"""
    return int(v)


def simulate(code, order, idx):
    """把 `order[idx]`（syscall / call）之前的实参块用栈机算出来。

    返回实参列表（pushfalse/pushtrue → None），算不出来返回 None。
    """
    j = idx
    while j > 0 and code.instr[order[j - 1]][1] in PURE:
        j -= 1
    st = []
    for k in range(j, idx):
        _op, nm, args = code.instr[order[k]]
        try:
            if nm in ('pushint8', 'pushint16', 'pushint32'):
                st.append(int(args[0]))
            elif nm == 'pushfloat':
                st.append(f32(args[0]))
            elif nm == 'pushstring':
                st.append(str(args[0]))
            elif nm in ('pushfalse', 'pushtrue'):
                st.append(None)
            elif nm == 'neg':
                st.append(-st.pop())
            elif nm == 'not':
                st.append(0 if st.pop() else 1)
            elif nm in ('add', 'sub', 'mul', 'div', 'mod', 'eq', 'neq', 'lt',
                        'gt', 'le', 'ge', 'logand', 'logor', 'bitand', 'bitor',
                        'bitxor', 'shl', 'shr'):
                b = st.pop()
                a = st.pop()
                numeric = isinstance(a, (int, float)) and \
                    isinstance(b, (int, float))
                is_float = isinstance(a, float) or isinstance(b, float)
                if nm == 'add':
                    r = a + b
                elif nm == 'sub':
                    r = a - b
                elif nm == 'mul':
                    r = a * b
                elif nm == 'div':
                    if isinstance(a, int) and isinstance(b, int) and b != 0:
                        r = c_trunc(a / b)
                    else:
                        r = a / b if b else 0
                elif nm == 'mod':
                    r = a % b if b else 0
                elif nm in ('eq', 'neq', 'lt', 'gt', 'le', 'ge'):
                    r = 1 if {'eq': a == b, 'neq': a != b, 'lt': a < b,
                             'gt': a > b, 'le': a <= b,
                             'ge': a >= b}[nm] else 0
                elif nm in ('logand', 'logor'):
                    r = (bool(a) and bool(b)) if nm == 'logand' else \
                        (bool(a) or bool(b))
                    r = 1 if r else 0
                elif nm == 'bitand':
                    r = int(a) & int(b)
                elif nm == 'bitor':
                    r = int(a) | int(b)
                elif nm == 'bitxor':
                    r = int(a) ^ int(b)
                elif nm == 'shl':
                    r = int(a) << int(b)
                else:  # shr
                    r = int(a) >> int(b)
                if is_float and numeric and nm in ('add', 'sub', 'mul', 'div'):
                    r = f32round(r)
                st.append(r)
            elif nm == 'test':
                st.append(1 if st.pop() else 0)
            else:
                return None
        except (IndexError, TypeError, ValueError, ZeroDivisionError):
            return None
    return st


def op_names():
    return {op: val[0] for op, val in fvm_ref.OPDEF.items()}


def decode_helper(code, order, names, helper_pc):
    """解 helper 的函数体：返回 (包装的 syscall id, 逻辑→HD 换算因子列表)。

    因子来自 helper 里 `pushstack; pushfloat; mul; syscall 20(FloatToInt)` 的
    固定模式（出现顺序 = 实参顺序）。
    """
    seq = []
    for pc in order:
        if pc < helper_pc:
            continue
        if pc > helper_pc + 0x400:
            break
        seq.append((pc, code.instr[pc]))
        if code.instr[pc][1] == 'ret':
            break
    wrapped = None
    factors = []
    for k, (_pc, (_op, nm, args)) in enumerate(seq):
        if nm == 'syscall':
            wrapped = args[0]
        if nm == 'pushstack' and k + 3 < len(seq):
            n1 = seq[k + 1][1]
            n2 = seq[k + 2][1]
            n3 = seq[k + 3][1]
            if (n1[1] == 'pushfloat' and n2[1] == 'mul' and n3[1] == 'syscall'
                    and n3[2] and n3[2][0] == 20):
                factors.append(f32(n1[2][0]))
    return wrapped, factors


def resolve_coords(cmd, raw, factors):
    """按 COORD_ARGS 把 helper 实参里的逻辑坐标换算成 HD 像素（截断）。"""
    out = list(raw)
    start, count = COORD_ARGS[cmd]
    for k in range(count):
        idx = start + k
        if idx < len(out) and isinstance(out[idx], (int, float)) and \
                k < len(factors):
            out[idx] = c_trunc(f32round(f32round(out[idx]) * factors[k]))
    return out


def read_index(path):
    """读 BIN 索引 → [(name, data_off, size)]（同 extract-title-rects.py）。"""
    with open(path, 'rb') as fp:
        head = fp.read(8)
        if len(head) < 8:
            return None
        count, name_len = struct.unpack('<II', head)
        if count == 0 or count > 100000:
            return None
        idx = fp.read(12 * count)
        if len(idx) < 12 * count:
            return None
        names = fp.read(name_len)
        if len(names) < name_len:
            return None
    out = []
    for i in range(count):
        name_off, data_off, size = struct.unpack_from('<III', idx, i * 12)
        if name_off >= name_len:
            continue
        end = names.find(b'\0', name_off)
        if end < 0:
            continue
        raw = names[name_off:end]
        try:
            name = raw.decode('cp932')
        except UnicodeDecodeError:
            name = raw.decode('cp932', 'replace')
        out.append((name, data_off, size))
    return out


def asset_header(game_dir, name):
    """从 graph.bin 的 HZC1 头读出 (w, h, x, y)；找不到返回 None。"""
    for pkg in ('graph.bin', 'graph_bg.bin'):
        path = os.path.join(game_dir, pkg)
        if not os.path.isfile(path):
            continue
        idx = read_index(path)
        if idx is None:
            continue
        for nm, off, size in idx:
            if nm != name or size < 44:
                continue
            with open(path, 'rb') as fp:
                fp.seek(off)
                hdr = fp.read(44)
            if hdr[:4] != b'hzc1':
                continue
            w, h, x, y = struct.unpack_from('<HHHH', hdr, 0x14)
            return {'w': w, 'h': h, 'x': x, 'y': y}
    return None


def find_loads(code, order):
    loads = []
    for i, pc in enumerate(order):
        if i < 4:
            continue
        win = [code.instr[order[i - k]][1] for k in range(4, 0, -1)]
        vals = [code.instr[order[i - k]][2] for k in range(4, 0, -1)]
        if win != ['pushint16', 'pushstring', 'pushint8', 'pushfalse']:
            continue
        if not vals[0] or not vals[1]:
            continue
        name = str(vals[1][0])
        if name not in OPEN_NAMES:
            continue
        loads.append({'slot': vals[0][0], 'name': name,
                      'kind': (vals[2][0] if vals[2] else None),
                      'pc': pc, 'call_pc': pc})
    return loads


def extract(hcb_path, game_dir, cmd_table, names):
    with open(hcb_path, 'rb') as fp:
        data = fp.read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)
    idx_of = {pc: i for i, pc in enumerate(order)}

    loads = find_loads(code, order)
    if not loads:
        raise RuntimeError('找不到开场 logo 装载点')
    first_load = min(x['pc'] for x in loads if x['name'] == 'logo_bg')
    start = None
    for pc in reversed(order):
        if pc >= first_load:
            continue
        if code.instr[pc][1] == 'initstack':
            start = pc
            break
    if start is None:
        raise RuntimeError('找不到开场函数入口（initstack）')

    end = None
    for pc in order:
        if pc <= first_load:
            continue
        _op, nm, args = code.instr[pc]
        if nm == 'popglobal' and args and args[0] == 9:
            end = pc
            break
    if end is None:
        raise RuntimeError('找不到开场函数结束标记（popglobal 9）')

    prim_setup = []
    motions = []
    helper_calls = []
    tests = []
    for pc in order:
        if pc < start or pc > end:
            continue
        idx = idx_of[pc]
        _op, nm, args = code.instr[pc]
        if nm == 'syscall' and args:
            cid = args[0]
            if cid not in MOTION_CMDS and cid not in PRIM_CMDS:
                continue
            vals = simulate(code, order, idx)
            if not vals or not isinstance(vals[0], int):
                continue
            prim = vals[0]
            if not (OPEN_PRIMS[0] <= prim <= OPEN_PRIMS[1]):
                continue
            rest = vals[1:]
            ent = {'prim': prim, 'cmd': cid,
                   'name': cmd_table.get(cid, {}).get('name', str(cid)),
                   'args': rest, 'pc': pc}
            if cid == 44:
                tests.append(ent)
            elif cid in PRIM_CMDS:
                prim_setup.append(ent)
            else:
                motions.append(ent)
        elif nm == 'call' and args:
            helper = args[0]
            wrapped, factors = decode_helper(code, order, names, helper)
            if wrapped not in COORD_ARGS:
                continue
            raw = simulate(code, order, idx)
            if not raw or not isinstance(raw[0], int):
                continue
            prim = raw[0]
            if not (OPEN_PRIMS[0] <= prim <= OPEN_PRIMS[1]):
                continue
            resolved = resolve_coords(wrapped, raw, factors)
            helper_calls.append({
                'prim': prim,
                'helper': '0x%08x' % helper,
                'wrapped': wrapped,
                'wrapped_name': cmd_table.get(wrapped, {}).get('name',
                                                               str(wrapped)),
                'raw_args': raw,
                'args': resolved,
                'pc': pc,
                'factors': factors,
            })

    # ---- 时间轴：全部从上面抽到的动画实参里推 ----
    alpha_in = {}
    alpha_out = {}
    cut_ms = None
    scale = {}
    rotate = {}
    move = {}
    move_z = {}
    op = {}
    layer_z = {}
    for m in motions:
        prim = m['prim']
        a = m['args']
        nm = m['name']
        if nm == 'MotionAlpha' and len(a) >= 3:
            frm, to, ms = a[0], a[1], a[2]
            if frm == 0 and to == 255:
                alpha_in[str(prim)] = ms
            elif frm is None and to == 0:
                if ms == 100:
                    cut_ms = ms
                else:
                    alpha_out.setdefault(str(prim), ms)
        elif nm == 'MotionMoveS2' and len(a) >= 6:
            # exe handler 0x43AF70：arg1=from_sx, arg2=to_sx, arg3=from_sy,
            # arg4=to_sy, arg5=dur, arg6=mode（两个轴交错，不是四元组）
            scale[str(prim)] = {'from_x': a[0], 'to_x': a[1],
                                'from_y': a[2], 'to_y': a[3],
                                'ms': a[4], 'mode': a[5]}
        elif nm == 'MotionMoveR' and len(a) >= 5:
            rotate[str(prim)] = {'from': a[0], 'to': a[1], 'ms': a[2],
                                 'mode': a[3]}
        elif nm == 'MotionMoveZ' and len(a) >= 4:
            move_z[str(prim)] = {'from': a[0], 'to': a[1], 'ms': a[2],
                                 'mode': a[3]}
    for h in helper_calls:
        a = h['args']
        if h['wrapped'] == 48 and len(a) >= 8:
            move[str(h['prim'])] = {
                'from_x': a[1] if isinstance(a[1], int) else 0,
                'from_y': a[2] if isinstance(a[2], int) else 0,
                'to_x': a[3] if isinstance(a[3], int) else 0,
                'to_y': a[4] if isinstance(a[4], int) else 0,
                'ms': a[5], 'mode': a[6],
                'raw_from_x': a[1], 'raw_from_y': a[2],
                'raw_to_x': a[3], 'raw_to_y': a[4],
            }
        elif h['wrapped'] == 81 and len(a) >= 3:
            op[str(h['prim'])] = [a[1], a[2]]
    for p in prim_setup:
        a = p['args']
        if p['cmd'] == 91 and len(a) >= 1:
            layer_z[str(p['prim'])] = a[0]
        elif p['cmd'] == 81 and len(a) >= 2:
            op[str(p['prim'])] = [a[0], a[1]]
    # 逻辑画布：logo_favo 的 OP 调用实参 ×2（脚本写的是逻辑画布中心）
    logical = None
    for h in helper_calls:
        if h['wrapped'] == 81 and h['prim'] == 252 and len(h['raw_args']) >= 3:
            rx, ry = h['raw_args'][1], h['raw_args'][2]
            if isinstance(rx, int) and isinstance(ry, int) and rx > 0:
                factors = h['factors']
                logical = {
                    'w': rx * 2, 'h': ry * 2,
                    'fx': factors[0] if len(factors) > 0 else None,
                    'fy': factors[1] if len(factors) > 1 else None,
                }
                break
    if logical is None and op.get('252'):
        logical = {'w': None, 'h': None, 'fx': None, 'fy': None}

    # 收尾时刻由脚本的 MotionAlphaTest(252) 决定：等 252 的**淡出**跑完才接 100 ms
    # 快速收尾（0x5843B 的 MotionAlphaTest(252)），所以总时长不是取最长的 3500
    # （白底）而是 1200 + 2500 + 100 = 3800 ms。
    out_start = alpha_in.get('252')
    cut_start = None
    total = None
    if out_start is not None and cut_ms is not None and '252' in alpha_out:
        cut_start = out_start + alpha_out['252']
        total = cut_start + cut_ms

    # ---- 素材头（尺寸/摆放坐标）：用于核对 OP 与画面位置 ----
    assets = {}
    for name in OPEN_NAMES:
        hdr = asset_header(game_dir, name)
        if hdr is not None:
            assets[name] = hdr

    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'opening_start': '0x%08x' % start,
        'opening_end': '0x%08x' % end,
        'loads': [{'slot': x['slot'], 'name': x['name'], 'kind': x['kind'],
                   'pc': '0x%08x' % x['pc']} for x in loads],
        'prim_setup': [{'prim': x['prim'], 'cmd': x['cmd'], 'name': x['name'],
                        'args': x['args'], 'pc': '0x%08x' % x['pc']}
                       for x in prim_setup],
        'motions': [{'prim': x['prim'], 'cmd': x['cmd'], 'name': x['name'],
                     'args': x['args'], 'pc': '0x%08x' % x['pc']}
                    for x in motions],
        'helper_calls': [{'prim': x['prim'], 'helper': x['helper'],
                          'wrapped': x['wrapped'],
                          'wrapped_name': x['wrapped_name'],
                          'raw_args': x['raw_args'], 'args': x['args'],
                          'factors': x['factors'], 'pc': '0x%08x' % x['pc']}
                         for x in helper_calls],
        'waits': [{'prim': x['prim'], 'name': x['name'], 'pc': '0x%08x' % x['pc']}
                  for x in tests],
        'logical': logical,
        'assets': assets,
        'timeline': {
            'alpha_in_ms': alpha_in,
            'alpha_out_ms': alpha_out,
            'cut_ms': cut_ms,
            'out_start_ms': out_start,
            'cut_start_ms': cut_start,
            'total_ms': total,
            'scale': scale,
            'rotate': rotate,
            'move': move,
            'move_z': move_z,
            'op': op,
            'z': layer_z,
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all',
                    choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    with io.open(os.path.join(ROOT, 'ref', 'cmd_table.json'),
                 encoding='utf-8') as fp:
        cmd_table = {e['id']: e for e in json.load(fp)}
    names = op_names()

    result = {
        'note': '开场 FAVORITE logo 序列的脚本侧事实（槽位/时长/坐标/轴心）；'
                '由 scripts/extract-opening-scene.py 生成，不含游戏数据本体',
        'games': {},
    }
    keys = sorted(GAMES) if opt.game == 'all' else [opt.game]
    for name in keys:
        g = GAMES[name]
        if not os.path.isfile(g['hcb']):
            print('SKIP: %s 不存在（%s）' % (name, g['hcb']))
            return 0
        r = extract(g['hcb'], g['dir'], cmd_table, names)
        result['games'][name] = r
        tl = r['timeline']
        print('%s  %s..%s  装载 %d 层  动画 %d 条  helper 调用 %d 条'
              % (name, r['opening_start'], r['opening_end'], len(r['loads']),
                 len(r['motions']), len(r['helper_calls'])))
        print('   logical=%s  op=%s' % (r['logical'], tl['op']))
        print('   alpha_in=%s  alpha_out=%s  out_start=%s  cut=%s  total=%s'
              % (tl['alpha_in_ms'], tl['alpha_out_ms'], tl['out_start_ms'],
                 tl['cut_ms'], tl['total_ms']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 提取结果与 ref/opening-scene.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/opening-scene.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())