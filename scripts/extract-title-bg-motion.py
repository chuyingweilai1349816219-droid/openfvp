#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从脚本里静态提取**标题背景层的缩放/旋转参数**（两个作品，不跑原版、不截图）。

回答的问题：「背景的旋转能靠解包解出数据吗？」——**能，但不在图像包里**：

  * `graph.bin` 的 HZC1 头只有 `w/h/x/y/fmt` 和像素，**没有旋转**；
  * 旋转/缩放是 HCB 脚本里的 `MotionMoveR`(51) / `MotionMoveS2`(54) 调用，
    实参是字面量（角度单位 0.1°，时长/缓动 mode 见 handler 0x43AE50 / 0x43AF70）。

本脚本把两作标题背景的调用点连同实参冻结下来（含"时长来自栈局部变量、而该局部
变量在不同分支取值不同"的情形，两个候选值都记下来，不猜）：

  五彩斑斓（hcb 0x5912F..0x591D3）：
    258 title_bg1_A  1300→900 缩放 + 150→0 旋转（25000 ms，mode 3）
    259 title_bg1_B  1600→800      + 250→0
    260 title_bg1_C  2500→800      + 550→0
    261 title_bg1_D  4000→800      + 1250→0
    OP 全部 (1200,675)（设计画布 2400×1350 中心），RS 缩放 1000。
  星空（hcb 0x6FD87 起）：
    背景只有槽 258（依次装 title_bg3 / title_bg2 / title_bg1，最后留下 title_bg1），
    只有一次 `MotionMoveS2 2000→1000`，**没有 MotionMoveR**（背景不旋转）。

产物 `ref/title-bg-motion.json`（只含槽位/参数/pc，不含游戏数据本体）。

用法：
    python scripts/extract-title-bg-motion.py            # 打印 + 写 json
    python scripts/extract-title-bg-motion.py --check    # 只比对（门禁用）
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
        'bg_slots': [258, 259, 260, 261],
        'region': (0x58500, 0x59200),
    },
    'hoshimemo': {
        'hcb': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                            'Hoshimemo_HD.hcb'),
        'dir': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
        'bg_slots': [258],
        'region': (0x6f000, 0x6fe00),
    },
}
OUT_JSON = os.path.join(ROOT, 'ref', 'title-bg-motion.json')
BOUNDARY = {'syscall', 'call', 'jmp', 'jmpcond', 'initstack', 'ret', 'ret2'}


def f32(bits):
    return struct.unpack('<f', struct.pack('<I', bits & 0xFFFFFFFF))[0]


def simulate(code, order, idx):
    """反扫到上一个非纯指令，整段模拟出值栈。"""
    pure = {'pushint8', 'pushint16', 'pushint32', 'pushfloat', 'pushstring',
            'pushtrue', 'pushfalse', 'pushstack', 'copystack', 'add', 'sub',
            'mul', 'div', 'neg', 'eq', 'neq', 'lt', 'gt', 'le', 'ge',
            'logand', 'logor', 'not', 'test'}
    j = idx
    while j > 0 and code.instr[order[j - 1]][1] in pure:
        j -= 1
    st = []
    locals_ = {}
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
            reg = args[0] if args else None
            st.append(locals_.get(reg, ('local', reg)))
        elif nm == 'copystack':
            locals_[args[0]] = st.pop()
        elif nm == 'neg':
            st.append(-st.pop())
        elif nm == 'not':
            st.append(0 if st.pop() else 1)
        elif nm in ('add', 'sub', 'mul', 'div'):
            b = st.pop()
            a = st.pop()
            if isinstance(a, tuple) or isinstance(b, tuple):
                return None
            st.append({'add': a + b, 'sub': a - b, 'mul': a * b,
                       'div': (int(a / b) if isinstance(a, int) and
                               isinstance(b, int) and b else 0)}[nm])
        elif nm in ('eq', 'neq', 'lt', 'gt', 'le', 'ge'):
            b = st.pop()
            a = st.pop()
            st.append(1 if {'eq': a == b, 'neq': a != b, 'lt': a < b,
                            'gt': a > b, 'le': a <= b,
                            'ge': a >= b}[nm] else 0)
        elif nm in ('logand', 'logor'):
            b = st.pop()
            a = st.pop()
            st.append(1 if ((bool(a) and bool(b)) if nm == 'logand'
                            else (bool(a) or bool(b))) else 0)
        elif nm == 'test':
            st.append(1 if st.pop() else 0)
        else:
            return None
    return st


def local_candidates(code, order, idx, reg, func_start):
    """在 [func_start, call pc) 里找 `pushintN k; copystack [reg]` 与
    `pushstack [base]; pushintN delta; add/sub; copystack [reg]` 两种赋值，
    返回所有候选值（同一 reg 可能被不同分支赋不同值）。"""
    out = []
    for k in range(len(order)):
        pc = order[k]
        if not (func_start <= pc < order[idx]):
            continue
        nm = code.instr[pc][1]
        args = code.instr[pc][2]
        if nm in ('pushint8', 'pushint16', 'pushint32') and k + 1 < len(order):
            nxt = code.instr[order[k + 1]]
            if nxt[1] == 'copystack' and nxt[2][0] == reg:
                out.append({'value': args[0], 'pc': '0x%08x' % pc})
        if nm == 'pushstack' and k + 3 < len(order):
            a = code.instr[order[k + 1]]
            b = code.instr[order[k + 2]]
            c = code.instr[order[k + 3]]
            if a[1] in ('pushint8', 'pushint16', 'pushint32') and \
                    b[1] in ('add', 'sub') and c[1] == 'copystack' and \
                    c[2][0] == reg:
                out.append({'expr': '%s %s %d' % (args[0], b[1], a[2][0]),
                            'pc': '0x%08x' % pc})
    return out


def func_start_of(code, order, idx):
    for k in range(idx, -1, -1):
        if code.instr[order[k]][1] == 'initstack':
            return order[k]
    return 0


def asset_headers(game_dir, names):
    out = {}
    for pkg in ('graph.bin', 'graph_bg.bin'):
        path = os.path.join(game_dir, pkg)
        if not os.path.isfile(path):
            continue
        with open(path, 'rb') as fp:
            head = fp.read(8)
            if len(head) < 8:
                continue
            count, name_len = struct.unpack('<II', head)
            idx = fp.read(12 * count)
            names_blob = fp.read(name_len)
        for i in range(count):
            noff, doff, size = struct.unpack_from('<III', idx, i * 12)
            end = names_blob.find(b'\0', noff)
            if end < 0:
                continue
            try:
                nm = names_blob[noff:end].decode('cp932')
            except UnicodeDecodeError:
                continue
            if nm not in names or size < 44:
                continue
            with open(path, 'rb') as fp:
                fp.seek(doff)
                hdr = fp.read(44)
            if hdr[:4] != b'hzc1':
                continue
            w, h, x, y = struct.unpack_from('<HHHH', hdr, 0x14)
            out[nm] = {'w': w, 'h': h, 'x': x, 'y': y, 'pkg': pkg}
    return out


def extract(name, cfg):
    data = open(cfg['hcb'], 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)
    idx = {pc: i for i, pc in enumerate(order)}
    with open(os.path.join(ROOT, 'ref', 'cmd_table.json'), encoding='utf-8') as fp:
        argc = {e['id']: e['argc'] for e in json.load(fp)}

    loads = []
    target_names = set()
    for pc in order:
        if not (cfg['region'][0] <= pc <= cfg['region'][1]):
            continue
        i = idx[pc]
        op, nm, args = code.instr[pc]
        if nm != 'call' or i < 4:
            continue
        window = [code.instr[order[i - k]][1] for k in range(4, 0, -1)]
        vals = [code.instr[order[i - k]][2] for k in range(4, 0, -1)]
        if window != ['pushint16', 'pushstring', 'pushint8', 'pushtrue']:
            continue
        slot = vals[0][0] if vals[0] else None
        asset = str(vals[1][0]) if vals[1] else ''
        if slot in cfg['bg_slots']:
            loads.append({'slot': slot, 'asset': asset,
                          'pc': '0x%08x' % pc})
            target_names.add(asset)

    ops = []
    motions = []
    for pc in order:
        if not (cfg['region'][0] <= pc <= cfg['region'][1]):
            continue
        i = idx[pc]
        op, nm, args = code.instr[pc]
        if nm != 'syscall' or not args:
            continue
        cid = args[0]
        if cid not in (51, 54, 81, 82, 91):
            continue
        vals = simulate(code, order, i)
        if not vals:
            continue
        n = argc.get(cid, 2)
        vals = vals[-n:]
        if not vals or not isinstance(vals[0], int):
            continue
        prim = vals[0]
        if prim not in cfg['bg_slots']:
            continue
        if cid in (81, 82, 91):
            ops.append({'prim': prim, 'cmd': cid, 'args': vals, 'pc': pc})
            continue
        fstart = func_start_of(code, order, i)
        resolved = []
        for a in vals:
            if isinstance(a, tuple) and a and a[0] == 'local':
                reg = a[1]
                cand = local_candidates(code, order, i, reg, fstart)
                resolved.append({'local': reg, 'candidates': cand})
            else:
                resolved.append(a)
        motions.append({
            'prim': prim, 'cmd': cid,
            'name': 'MotionMoveS2' if cid == 54 else 'MotionMoveR',
            'args_resolved': resolved,
            'pc': '0x%08x' % pc,
        })

    headers = asset_headers(cfg['dir'], target_names)
    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'bg_slots': cfg['bg_slots'],
        'region': ['0x%08x' % cfg['region'][0], '0x%08x' % cfg['region'][1]],
        'loads': loads,
        'assets': headers,
        'prim_ops': [{'prim': x['prim'], 'cmd': x['cmd'], 'args': x['args'],
                      'pc': '0x%08x' % x['pc']} for x in ops],
        'motions': [{'prim': x['prim'], 'cmd': x['cmd'], 'name': x['name'],
                     'args': x['args_resolved'],
                     'pc': x['pc']} for x in motions],
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    result = {
        'note': '标题背景层的缩放/旋转参数：旋转来自 HCB 的 MotionMoveR(51)，'
                '图像包（HZC1）只有 w/h/x/y/fmt，没有旋转信息；'
                '由 scripts/extract-title-bg-motion.py 生成，不含游戏数据本体',
        'games': {},
    }
    keys = sorted(GAMES) if opt.game == 'all' else [opt.game]
    for name in keys:
        cfg = GAMES[name]
        if not os.path.isfile(cfg['hcb']):
            print('SKIP: %s 不存在' % cfg['hcb'])
            return 0
        r = extract(name, cfg)
        result['games'][name] = r
        print('%s  bg_slots=%s  loads=%d  prim_ops=%d  motions=%d'
              % (name, r['bg_slots'], len(r['loads']), len(r['prim_ops']),
                 len(r['motions'])))
        for m in r['motions']:
            print('  %s prim=%d args=%r' % (m['name'], m['prim'], m['args']))

    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/title-bg-motion.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-bg-motion.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())