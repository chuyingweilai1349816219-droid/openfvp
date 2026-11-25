#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从脚本里静态提取**标题进场的逐元素时间轴**（不跑原版、不截图）。

背景：`src/titleanim.h` 以前把标题进场写成"统一的 500 ms 淡入"。那个 500 来自
hcb 0x59120 的 `local5`，而那一处只被**背景动画**用到；标题对象真正的进场在
0x592C7 被重设成 `local5=2000 / local7=3000`，菜单项是 `local5=900 / local7=820`。
本轮把进场块 0x5933E..0x5961A 整段解出来：

  等待 1000 ms
  264  alpha →255 / 2000 ms                                  ← 装饰层
  等待 1000 ms
  265  0→255 / 2000 ms（标题 logo）
  263  0→255 / 2000 ms + 纵向 -28 逻辑像素 → 0                ← 装饰层
  262  0→255 / 3000 ms                                        ← 装饰层
  菜单项（等待 400 ms 后每项 +40 ms）：
    1501 0→255 / 900 ms + 纵向 +14 逻辑像素 → 0
    1508/1515/1522/1529 0→255 / 820 ms + 纵向 +14 → 0
  278  0→255 / 820 ms + 横向 +16 逻辑像素 → 0（版权行）
  ⇒ 总时长 4000 ms（logo 最后结束）

逻辑像素按本作 helper 的 float32 因子（1.875 / 1.6875）换成 HD 像素，与原版一致。

产物 `ref/title-enter.json`（只含时长/偏移/prim id，不含游戏数据本体）。

用法：
    python scripts/extract-title-enter.py            # 打印 + 写 json
    python scripts/extract-title-enter.py --check    # 只比对（门禁用）
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

# 进场块的 pc 边界（iroseka；由人工定位后写死，下面会做形状校验）
START_PC = 0x59335
END_PC = 0x59650  # 含 0x59643 的 MotionAlphaTest(278)
# 装饰层（本站尚未绘制）与"要画"的元素
FRAME_PRIMS = {262: 'title_frame_a', 263: 'title_frame_b', 264: 'title_frame_c'}
DRAWN = {265: 'logo', 278: 'copyright',
         1501: 'menu1', 1508: 'menu2', 1515: 'menu3', 1522: 'menu4',
         1529: 'menu5'}
# 逻辑像素 → HD 像素的 float32 因子（本作 helper 里的常量）
FX_BITS = 0x3FF00000  # 1.875f
FY_BITS = 0x3FD80000  # 1.6875f
# local 变量的取值段：pc < 0x593D9 用 A 段，否则 B 段
LOCAL_BLOCK_A = {5: 2000, 7: 3000}
LOCAL_BLOCK_A_PC = 0x592C7
LOCAL_BLOCK_B = {5: 900, 7: 820, 10: 14}
LOCAL_BLOCK_B_PC = 0x593D9
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                    'iroseka.hcb')
OUT_JSON = os.path.join(ROOT, 'ref', 'title-enter.json')

PURE = {
    'pushint8', 'pushint16', 'pushint32', 'pushfloat', 'pushstring',
    'pushtrue', 'pushfalse',
    'add', 'sub', 'mul', 'div', 'mod', 'neg',
    'eq', 'neq', 'lt', 'gt', 'le', 'ge', 'logand', 'logor', 'not', 'test',
    'bitand', 'bitor', 'bitxor', 'shl', 'shr',
}


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def f32round(v):
    return struct.unpack('<f', struct.pack('<f', float(v)))[0]


def c_trunc(v):
    return int(v)


def simulate(code, order, idx, locals_at):
    """从上一个"非纯"指令之后整段模拟到 idx，返回整个值栈。

    （不能只反扫 PURE：MotionAlpha 的实参中间夹着 `pushstack [5]` 这种读局部，
    反扫会在那里停住，把 prim 漏掉。）
    """
    boundary = {'syscall', 'call', 'jmp', 'jmpcond', 'initstack', 'ret',
                'ret2', 'retn'}
    j = idx
    while j > 0 and code.instr[order[j - 1]][1] not in boundary:
        j -= 1
    st = []
    for k in range(j, idx):
        _op, nm, args = code.instr[order[k]]
        if nm in ('pushint8', 'pushint16', 'pushint32'):
            st.append(int(args[0]))
        elif nm == 'pushfloat':
            st.append(f32(args[0]))
        elif nm == 'pushstring':
            st.append(str(args[0]))
        elif nm in ('pushtrue', 'pushfalse'):
            st.append(None)
        elif nm == 'pushstack':
            v = locals_at(order[k], args[0] if args else None)
            if v is None:
                raise RuntimeError('unknown local at 0x%x' % order[k])
            st.append(v)
        elif nm == 'neg':
            st.append(-st.pop())
        elif nm == 'not':
            st.append(0 if st.pop() else 1)
        elif nm in ('add', 'sub', 'mul', 'div', 'eq', 'neq', 'lt', 'gt', 'le',
                    'ge', 'logand', 'logor', 'bitand', 'bitor', 'bitxor',
                    'shl', 'shr'):
            b = st.pop()
            a = st.pop()
            is_float = isinstance(a, float) or isinstance(b, float)
            if nm == 'add':
                r = a + b
            elif nm == 'sub':
                r = a - b
            elif nm == 'mul':
                r = a * b
            elif nm == 'div':
                if isinstance(a, int) and isinstance(b, int) and b:
                    r = c_trunc(a / b)
                else:
                    r = a / b if b else 0
            elif nm in ('eq', 'neq', 'lt', 'gt', 'le', 'ge'):
                r = 1 if {'eq': a == b, 'neq': a != b, 'lt': a < b,
                          'gt': a > b, 'le': a <= b,
                          'ge': a >= b}[nm] else 0
            elif nm in ('logand', 'logor'):
                r = 1 if ((bool(a) and bool(b)) if nm == 'logand'
                          else (bool(a) or bool(b))) else 0
            elif nm == 'bitand':
                r = int(a) & int(b)
            elif nm == 'bitor':
                r = int(a) | int(b)
            elif nm == 'bitxor':
                r = int(a) ^ int(b)
            elif nm == 'shl':
                r = int(a) << int(b)
            else:
                r = int(a) >> int(b)
            if is_float and nm in ('add', 'sub', 'mul', 'div'):
                r = f32round(r)
            st.append(r)
        elif nm == 'test':
            st.append(1 if st.pop() else 0)
        else:
            raise RuntimeError('unexpected op %s at 0x%x' % (nm, order[k]))
    return st


def args_n(code, order, idx, locals_at, n):
    """整段模拟后取最后 n 个值（不足返回 None）。"""
    vals = simulate(code, order, idx, locals_at)
    if len(vals) < n:
        return None
    return vals[-n:]


def logical_to_hd(v, bits):
    return c_trunc(f32round(f32round(v) * f32(bits)))


def find_local_block(code, order, pc_from, expect):
    """校验某段里的 local 赋值，返回起始 pc。

    两种形态都认（hcb 里两种都有）：
      - `pushintN k; copystack [j]`（字面量）
      - `pushstack [base]; pushintN delta; add; copystack [j]`（base + delta）
    """
    literal = {}
    arith = {}
    for i, pc in enumerate(order):
        if pc < pc_from or pc > pc_from + 0x80:
            continue
        nm = code.instr[pc][1]
        args = code.instr[pc][2]
        if nm in ('pushint8', 'pushint16', 'pushint32') and \
                i + 1 < len(order):
            nxt = code.instr[order[i + 1]]
            if nxt[1] == 'copystack':
                literal[nxt[2][0]] = args[0]
        if nm == 'pushstack' and i + 3 < len(order):
            a = code.instr[order[i + 1]]
            b = code.instr[order[i + 2]]
            c = code.instr[order[i + 3]]
            if a[1] in ('pushint8', 'pushint16', 'pushint32') and \
                    b[1] in ('add', 'sub') and c[1] == 'copystack':
                arith[c[2][0]] = (args[0], a[2][0], b[1])
    for k, v in expect.items():
        if literal.get(k) == v:
            continue
        if k in arith:
            base, delta, op = arith[k]
            want = expect[base] + delta if op == 'add' else expect[base] - delta
            if base in expect and want == v:
                continue
        raise RuntimeError('local %d 期望 %d，字面量=%r 加法=%r（@0x%x）'
                           % (k, v, literal.get(k), arith.get(k), pc_from))
    return pc_from


def extract():
    data = open(GAME, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)
    idx = {pc: i for i, pc in enumerate(order)}

    find_local_block(code, order, LOCAL_BLOCK_A_PC, LOCAL_BLOCK_A)
    find_local_block(code, order, LOCAL_BLOCK_B_PC, LOCAL_BLOCK_B)

    def locals_at(pc, reg):
        table = LOCAL_BLOCK_A if pc < LOCAL_BLOCK_B_PC else LOCAL_BLOCK_B
        return table.get(reg)

    # 形状校验：起点是"等 1000 ms"，块里必须有 265 的淡入与 278 的测试
    head = code.instr.get(START_PC)
    if head is None or head[1] != 'pushint16' or head[2][0] != 1000:
        raise RuntimeError('进场块起点形状变了（0x%x）' % START_PC)
    pcs = [pc for pc in order if START_PC <= pc <= END_PC]
    if not pcs:
        raise RuntimeError('进场块为空')

    events = []
    t = 0
    saw_265 = False
    saw_278 = False
    for pc in pcs:
        i = idx[pc]
        _op, nm, args = code.instr[pc]
        if nm == 'call' and args and args[0] == 145552:
            vals = args_n(code, order, i, locals_at, 2)
            ms = vals[0] if vals else 0
            if isinstance(ms, int) and ms > 0:
                t += ms
                events.append({'kind': 'wait', 'ms': ms, 'at_ms': t - ms,
                               'pc': '0x%08x' % pc})
        elif nm == 'syscall' and args and args[0] == 42:
            vals = args_n(code, order, i, locals_at, 6)
            if not vals or not isinstance(vals[0], int):
                continue
            prim = vals[0]
            if prim not in DRAWN and prim not in FRAME_PRIMS:
                continue
            frm, to, dur = vals[1], vals[2], vals[3]
            events.append({
                'kind': 'alpha', 'prim': prim, 'from': frm, 'to': to,
                'duration_ms': dur, 'start_ms': t, 'at_ms': t,
                'pc': '0x%08x' % pc})
            saw_265 = saw_265 or prim == 265
        elif nm == 'call' and args and args[0] == 219703:
            vals = args_n(code, order, i, locals_at, 8)
            if not vals or not isinstance(vals[0], int):
                continue
            prim = vals[0]
            if prim not in DRAWN and prim not in FRAME_PRIMS:
                continue
            fx, fy = vals[1], vals[2]
            tx, ty = vals[3], vals[4]
            events.append({
                'kind': 'move', 'prim': prim,
                'from_x_logical': fx, 'from_y_logical': fy,
                'to_x_logical': tx, 'to_y_logical': ty,
                'from_dx': (logical_to_hd(fx, FX_BITS)
                            if isinstance(fx, int) else None),
                'from_dy': (logical_to_hd(fy, FY_BITS)
                            if isinstance(fy, int) else None),
                'to_dx': (logical_to_hd(tx, FX_BITS)
                          if isinstance(tx, int) else None),
                'to_dy': (logical_to_hd(ty, FY_BITS)
                          if isinstance(ty, int) else None),
                'duration_ms': vals[5], 'mode': vals[6], 'start_ms': t,
                'at_ms': t, 'pc': '0x%08x' % pc})
        elif nm == 'syscall' and args and args[0] == 44:
            vals = args_n(code, order, i, locals_at, 1)
            if vals and vals[0] == 278:
                saw_278 = True

    if not saw_265 or not saw_278:
        raise RuntimeError('进场块里没找到 265 淡入 / 278 测试，边界可能变了')

    elements = {}
    for e in events:
        if e['kind'] == 'alpha':
            elements.setdefault(str(e['prim']), {})['alpha'] = {
                'from': e['from'], 'to': e['to'],
                'start_ms': e['start_ms'], 'duration_ms': e['duration_ms'],
                'pc': e['pc']}
        elif e['kind'] == 'move':
            d = elements.setdefault(str(e['prim']), {})
            move = {
                'from_dx': e['from_dx'], 'from_dy': e['from_dy'],
                'to_dx': e['to_dx'], 'to_dy': e['to_dy'],
                'start_ms': e['start_ms'], 'duration_ms': e['duration_ms'],
                'mode': e['mode'], 'pc': e['pc']}
            # 1522/1529 有 global3 分支的第二条 move（终点 -48 逻辑像素 = -81 px）。
            # 本站实现"回到 0"的那条；另一条只记录，不实现。
            zero = ((move['to_dx'] in (0, None)) and
                    (move['to_dy'] in (0, None)))
            if 'move' not in d:
                d['move'] = move
            elif zero:
                d.setdefault('move_alternatives', []).append(d['move'])
                d['move'] = move
            else:
                d.setdefault('move_alternatives', []).append(move)

    total = 0
    total_drawn = 0
    for prim, d in elements.items():
        drawn = int(prim) in DRAWN
        for kind in ('alpha', 'move'):
            ev = d.get(kind)
            if ev is None:
                continue
            end = ev['start_ms'] + ev['duration_ms']
            total = max(total, end)
            if drawn:
                total_drawn = max(total_drawn, end)

    for prim in elements:
        elements[prim]['name'] = DRAWN.get(int(prim)) or FRAME_PRIMS.get(
            int(prim)) or '?'
        elements[prim]['drawn_by_engine'] = int(prim) in DRAWN

    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'entrance_start': '0x%08x' % START_PC,
        'entrance_end': '0x%08x' % END_PC,
        'logical_to_hd': {'fx': f32(FX_BITS), 'fy': f32(FY_BITS),
                          'note': '原版 Variant 的 float 是 float32（flds/fstps）'},
        'waits': [e for e in events if e['kind'] == 'wait'],
        'elements': elements,
        'total_ms': total,
        'total_ms_drawn': total_drawn,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(GAME):
        print('SKIP: 缺 %s' % GAME)
        return 0
    result = {
        'note': '标题进场逐元素时间轴（脚本 0x5933E..0x5961A）；'
                '由 scripts/extract-title-enter.py 生成，不含游戏数据本体',
        'games': {'iroseka': extract()},
    }
    r = result['games']['iroseka']
    print('iroseka  %s..%s  total=%s ms  elements=%d'
          % (r['entrance_start'], r['entrance_end'], r['total_ms'],
             len(r['elements'])))
    for prim in sorted(r['elements'], key=int):
        d = r['elements'][prim]
        a = d.get('alpha') or {}
        m = d.get('move') or {}
        print('  prim %-5s %-14s alpha %s..%s @%s/%s  move dx %s->%s dy %s->%s'
              % (prim, d['name'], a.get('from'), a.get('to'),
                 a.get('start_ms'), a.get('duration_ms'),
                 m.get('from_dx'), m.get('to_dx'), m.get('from_dy'),
                 m.get('to_dy')))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/title-enter.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-enter.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())