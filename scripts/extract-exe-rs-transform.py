#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从原版 exe 里静态提取"带缩放/旋转的图元（RS 分支）"的变换事实。

为什么要有它：标题背景要用 OP 轴心缩放 + 旋转，而**角度单位**我们之前记成
"0.1°/单位"，这次逐指令对出来是 **0.05°/单位**（渲染角 = 角度单位 × π / 3600）。

冻结的事实（地址都是 VA，影子基址 0x400000）：

A. 旋转角（图元 +0x24，16 位）
   * 写入口 `PrimSetRS`(82) handler 0x43BAF0：实参 → `idiv 0xE10(3600)` 取**余数**、
     负数补 +3600 后写 WORD [prim+0x24]（bytes `bf 10 0e 00 00` + `f7 ff`）；
   * 渲染处 0x42B1DE..0x42B1EE：`fild 角度` → `fdiv qword 0x45D778`（**3600.0**）→
     `fmul qword 0x472E20`（**π**，按 double 存的 float 版圆周率）→
     `call 0x446750`（D3DXMatrixRotationZ，参数是**弧度**）；
   * ⇒ 渲染角 = a·π/3600 rad = **a·0.05°**，7200 单位 = 360°。
     脚本层的 3600 是取模周期（`PrimSetRS` 里写的），和渲染角的"每圈 7200"不是一回事，
     两个事实都记下来，不合并。

B. RS 分支的矩阵合成（0x42B21E 起，`test byte [prim+6], 4`）
   * 四边形局部坐标 = (图头 xy − OP)（OP 分支 0x42AF08 已冻结过）；
   * `T1 = (prim+0x14, prim+0x16) + 累加平移`（0x42B228..0x42B25A）；
   * 依次：`D3DXMatrixRotationZ`(0x446750) → `Multiply`(0x446732) →
     `Translation`(0x44674A，T1) → `Multiply` → `Scaling`(0x446744) → `Multiply` →
     `Translation`(0x44674A，T2) → `Multiply`；
   * **T2 = (engine+0x60 / 2, engine+0x64 / 2)**（0x42B343..0x42B37D：
     读 `[engine+0x64]`、`[engine+0x60]` 再算术右移 1 位），即**画面中心**；
   * D3D 行向量约定（M 由 I 起依次右乘 R、T1、S、T2）⇒
     `最终 = T2 + S·(R·(图头 − OP) + prim + accum)`。

C. 该公式的两个自洽特例（用来交叉验证，不是新的证据）
   * 背景层（prim=0、accum=0、rotation=0、OP=图心）：s=1 时
     `最终 = 画面中心 − OP = (1920−2400)/2, (1080−1350)/2 = (−240,−135)`
     —— 正是本工程一直用的"超尺寸图居中裁剪 240/135"；
     s=0.8 时 `= (0,0)`（整屏铺满）、s=0.9 时 `= (−120,−67.5)`。
   * prim.xy≠0 的图元（开场 p1..p3）会额外带一个"画面中心 − OP"的偏移；
     这一条**尚未**接进渲染器（我们目前用 prim + OP + R·S·(图头−OP)），
     记在这里等实现时再验证，不悄悄当成已知。

D. 视口三连（engine+0x6A6A4C/50/54）
   0x424010 从某个对象 +0xA/+0xC/+0xE 写入；渲染里与 qword 0x45D578（1000.0）
   一起参与平移修正（0x42B249.. / 0x42B286..）。默认 0 时不影响上面的公式。

产物 `ref/exe-rs-transform.json`（常量/地址/指令字节/推导，不含游戏代码）。

用法：
    python scripts/extract-exe-rs-transform.py            # 打印 + 写 json
    python scripts/extract-exe-rs-transform.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__))) \
    if False else None  # 占位，下面重新算
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-rs-transform.json')
BASE = 0x400000


def load_exe(path):
    with open(path, 'rb') as fp:
        data = fp.read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    sec_off = opt + struct.unpack_from('<H', data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', data, off + 8)
        secs.append((name, vaddr, vsize, roff, rsize))
    return data, secs


def foff(secs, va):
    rva = va - BASE
    for _name, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return roff + (rva - vaddr)
    raise SystemExit('VA 0x%x 不在任何节里' % va)


def u8(data, secs, va, n):
    fo = foff(secs, va)
    return data[fo:fo + n]


def f64(data, secs, va):
    return struct.unpack('<d', u8(data, secs, va, 8))[0]


def extract(path):
    data, secs = load_exe(path)

    rot_div = f64(data, secs, 0x45D778)
    rot_mul = f64(data, secs, 0x472E20)
    viewport_coeff = f64(data, secs, 0x45D578)

    return {
        'exe': {
            'sha256': hashlib.sha256(data).hexdigest(),
            'size': len(data),
            'image_base': '0x%08x' % BASE,
        },
        'angle': {
            'field': 'prim+0x24 (int16)',
            'setter': {
                'handler': '0x0043baf0',
                'cmd': 82,
                'name': 'PrimSetRS',
                'modulus': int(rot_div),
                'modulus_bytes': u8(data, secs, 0x43BB1E, 5).hex(),
                'idiv_bytes': u8(data, secs, 0x43BB23, 2).hex(),
                'note': '实参对 3600 取余，负数 +3600 后写 WORD [prim+0x24]',
            },
            'renderer': {
                'pc_fild': '0x0042b1de',
                'pc_fdiv': '0x0042b1e2',
                'pc_fmul': '0x0042b1e8',
                'divisor': rot_div,
                'divisor_va': '0x0045d778',
                'divisor_bytes': u8(data, secs, 0x42B1E2, 6).hex(),
                'multiplier': rot_mul,
                'multiplier_va': '0x00472e20',
                'multiplier_bytes': u8(data, secs, 0x42B1E8, 6).hex(),
                'matrix_fn': '0x00446750 (D3DXMatrixRotationZ)',
                'matrix_call_bytes': u8(data, secs, 0x42B1FA, 5).hex(),
                'radians_formula': 'radians = units * %r / %r' % (rot_mul,
                                                                  rot_div),
                'degrees_per_unit': rot_mul / rot_div * 180.0 / 3.141592653589793,
                'units_per_turn': 360.0 / (rot_mul / rot_div * 180.0 /
                                           3.141592653589793),
            },
            'ambiguity': 'PrimSetRS 以 3600 取模（脚本层周期），渲染角是 0.05°/单位'
                         '（每圈 7200）；两者都冻结，渲染以渲染公式为准',
        },
        'rs_branch': {
            'gate_pc': '0x0042b21e',
            'gate_bytes': u8(data, secs, 0x42B21E, 6).hex(),
            'gate': 'test byte [prim+0x6], 4（RS/缩放脏位）',
            'local_quad': '(图头 xy) − OP（OP 在 prim+0x20/+0x22）',
            't1': '(prim+0x14, prim+0x16) + 累加平移',
            't2': '(engine+0x60 / 2, engine+0x64 / 2) = 画面中心',
            't2_bytes': u8(data, secs, 0x42B343, 0x20).hex(),
            'matrix_calls': [
                {'order': 1, 'fn': '0x00446750', 'name': 'RotationZ',
                 'pc': '0x0042b1fa', 'bytes': u8(data, secs, 0x42B1FA, 5).hex()},
                {'order': 2, 'fn': '0x00446732', 'name': 'Multiply',
                 'pc': '0x0042b20f', 'bytes': u8(data, secs, 0x42B20F, 5).hex()},
                {'order': 3, 'fn': '0x0044674a', 'name': 'Translation(T1)',
                 'pc': '0x0042b2b1', 'bytes': u8(data, secs, 0x42B2B1, 5).hex()},
                {'order': 4, 'fn': '0x00446732', 'name': 'Multiply',
                 'pc': '0x0042b2c6', 'bytes': u8(data, secs, 0x42B2C6, 5).hex()},
                {'order': 5, 'fn': '0x00446744', 'name': 'Scaling',
                 'pc': '0x0042b329', 'bytes': u8(data, secs, 0x42B329, 5).hex()},
                {'order': 6, 'fn': '0x00446732', 'name': 'Multiply',
                 'pc': '0x0042b33e', 'bytes': u8(data, secs, 0x42B33E, 5).hex()},
                {'order': 7, 'fn': '0x0044674a', 'name': 'Translation(T2)',
                 'pc': '0x0042b381', 'bytes': u8(data, secs, 0x42B381, 5).hex()},
                {'order': 8, 'fn': '0x00446732', 'name': 'Multiply',
                 'pc': '0x0042b396', 'bytes': u8(data, secs, 0x42B396, 5).hex()},
            ],
            'formula': 'final = T2 + S·(R·(img − OP) + prim + accum)',
            'convention': 'D3D 行向量：M 从 I 起依次右乘 R、T1、S、T2',
            'self_checks': [
                '背景 prim=0/R=I：s=1 → 画面中心 − OP = (−240,−135) '
                '= 超尺寸图居中裁剪（与冻结的 title bg 裁剪一致）',
                's=0.8 → (0,0) 整屏铺满；s=0.9 → (−120,−67.5)',
            ],
            'path_a': {
                'when': 'test byte [prim+0x6], 0x4 成立（PrimSetRS/RS2 设过）',
                'pc': '0x0042b21e',
                'bytes': u8(data, secs, 0x42B21E, 6).hex(),
                'formula': 'final = center + S·(R·(img − OP) + prim + accum)',
                'anchor': '画面中心（engine+0x60/2, engine+0x64/2）',
            },
            'path_b': {
                'when': '同一处 test 不成立（只被 MotionMoveS2 改过 scale、没设 RS 标志）',
                'pc': '0x0042b3a3',
                'bytes': u8(data, secs, 0x42B3A3, 0x18).hex(),
                'scale_pc': '0x0042b3fa',
                'scale_call_bytes': u8(data, secs, 0x42B3FA, 5).hex(),
                'op_flag_test_pc': '0x0042b41f',
                'op_flag_test_bytes': u8(data, secs, 0x42B41F, 6).hex(),
                'translation_op_pc': '0x0042b462',
                'translation_noop_pc': '0x0042b4c3',
                'formula': 'final = prim + anchor + S·R·(img − anchor)，''anchor = OP（OP 标志置位时；未置位时按图头偏移 0 处理）',
                'anchor': 'OP（不是画面中心）；没有 PrimSetOP 的图元等价于"以图头为原点"',
                'self_check': 'hoshimemo 背景（img=(0,0)、OP 未设、prim=(0,0)）'
                              ' ⇒ final = S·(0,0) = (0,0)，即**左上锚缩放**；'
                              '与 src/gfx.c 的 gfx_draw_image_zoomed 一致',
            },
            'rs_flag_evidence': {
                'iroseka': '脚本 PrimSetRS 只对 prim 258 调过（4 次，'
                           '见 ref/title-bg-motion.json 的 prim_ops）'
                           ' ⇒ 258 走 path A，259/260/261 走 path B',
                'hoshimemo': '没有任何 PrimSetOP/PrimSetRS ⇒ path B（左上锚）',
            },
            'not_used_yet': 'prim.xy≠0 的图元会多出"画面中心 − OP"的偏移'
                            '（开场 p1..p3 等）；渲染器目前仍用 '
                            'prim + OP + R·S·(img−OP)，等接这部分时再验证',
        },
        'viewport': {
            'fields': ['engine+0x6a6a4c', 'engine+0x6a6a50', 'engine+0x6a6a54'],
            'writer_pc': '0x00424010',
            'writer_bytes': u8(data, secs, 0x424010, 0x30).hex(),
            'coefficient': viewport_coeff,
            'coefficient_va': '0x0045d578',
            'note': '默认 0 时不影响上面的公式；系数 1000.0 与缩放参数同一量纲',
        },
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在（可选语料）' % EXE)
        return 0
    result = extract(EXE)
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    a = result['angle']['renderer']
    print('angle: /%r * %r rad ⇒ %.6f°/单位，每圈 %r 单位'
          % (a['divisor'], a['multiplier'], a['degrees_per_unit'],
             a['units_per_turn']))
    print('rs_branch: %s' % result['rs_branch']['formula'])
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/exe-rs-transform.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(old.splitlines(),
                                                  text.splitlines(),
                                                  'ref', 'new',
                                                  lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/exe-rs-transform.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())