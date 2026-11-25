#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 原版程序里抄出 D3D9 **渲染状态常量**（A 类事实，不跑原版、不抓帧）。

为什么需要它：第 15 轮把"背景四层怎么合成"用 54 个候选在原版帧上比过，结论是
"直通 alpha 的 over 最优"——但那是**拟合出来的**。用户的方法论要求：能靠静态分析
解决的事实一律先结构分析。真正回答"原版用什么混合"的地方在 exe 的
`SetRenderState` 调用点：`D3DRS_SRCBLEND` / `D3DRS_DESTBLEND` /
`D3DRS_ALPHABLENDENABLE` / `D3DRS_BLENDOP` 的实参就是答案，抓帧拿不到。

做法（不猜，只抄）：

  1. 解析 `work/iroseka_HD.asm` 的指令行；
  2. D3D9 是 COM，设备方法走 vtable，典型形态：
         mov  0x8(%esi),%eax      ; eax = 设备对象（HGraph 的成员）
         mov  (%eax),%ecx         ; ecx = vtable
         mov  0xe4(%ecx),%edx     ; edx = vtable[0xE4/4]
         push $value              ; 第 3 个实参（__stdcall 右到左）
         push $state              ; 第 2 个实参
         push %eax                ; 第 1 个实参 = this
         call *%edx
     所以：找到 `call *%reg`，往前找加载同一寄存器的 `mov 0xOFF(...),%reg`，
     得到 vtable 偏移 OFF；再往回收立即数 `push $imm`。
  3. **值可能来自分支**（同一调用点、两个 push 走不同分支），这种站点收多个候选值，
     标 `conditional: true`，不假装它只有一个值。
  4. 偏移 → 方法名的映射、state/value → D3D 枚举名的映射是 D3D9 SDK 的公开常量
     （不是游戏数据，见本文件 VTABLE / D3DRS / D3DBLEND / D3DBLENDOP / D3DCMP）。

覆盖率的判定标准是硬的：asm 里 `mov 0xe4(...)` 一共出现多少次（36 次），
本脚本能解析出 (state, value) 的有多少、解析不出的（实参来自寄存器或更远的分支）
记进 `unresolved_sites`，不做推测。

产物 `ref/exe-d3d-states.json`（只含参数，不含游戏数据）。

用法：
    python scripts/extract-exe-d3d-states.py            # 打印 + 写 json
    python scripts/extract-exe-d3d-states.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-d3d-states.json')

VT_OFF = 0xE4          # 观察到的 SetRenderState vtable 偏移（全部 36 次 mov 0xe4(...)）

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
VT_MOV_RE = re.compile(r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
CALL_RE = re.compile(r'^call\s+\*%e([a-z]{2})$')
PUSH_IMM_RE = re.compile(r'^push\s+\$0x([0-9a-f]+)$')
OBJ_RE = re.compile(r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
BRANCH_RE = re.compile(r'^j\w+\b')

# vtable 偏移 → 方法名。**只有 0xE4 这一条是"实参形态+常量集合"双重验证过的**，
# 其余只是便于人看，门禁不用它们。
VTABLE = {
    0x44: 'Present?', 0xA0: 'BeginScene?', 0xA4: 'EndScene?', 0xA8: 'Clear?',
    0xAC: 'SetTransform?', 0xB8: 'SetViewport?', 0xC0: 'SetMaterial?',
    0xE0: '(unused)', 0xE4: 'SetRenderState',
    0x104: 'SetTexture?', 0x10C: 'SetTextureStageState?',
    0x114: 'SetSamplerState?', 0x14C: 'SetStreamSource?', 0x164: 'SetPixelShader?',
}

# D3DRENDERSTATETYPE（D3D9 SDK d3d9types.h 公开枚举值）
D3DRS = {
    7: 'ZENABLE', 8: 'FILLMODE', 9: 'SHADEMODE', 10: 'LINEPATTERN',
    14: 'ZWRITEENABLE', 15: 'ALPHATESTENABLE', 16: 'LASTPIXEL',
    19: 'SRCBLEND', 20: 'DESTBLEND', 22: 'CULLMODE', 23: 'ZFUNC',
    24: 'ALPHAREF', 25: 'ALPHAFUNC', 26: 'DITHERENABLE',
    27: 'ALPHABLENDENABLE', 28: 'FOGENABLE', 29: 'SPECULARENABLE',
    34: 'FOGCOLOR', 35: 'FOGTABLEMODE', 36: 'FOGSTART', 37: 'FOGEND',
    38: 'FOGDENSITY', 48: 'RANGEFOGENABLE', 52: 'STENCILENABLE',
    136: 'CLIPPING', 137: 'LIGHTING', 139: 'AMBIENT', 140: 'FOGVERTEXMODE',
    141: 'COLORVERTEX', 142: 'LOCALVIEWER', 143: 'NORMALIZENORMALS',
    145: 'DIFFUSEMATERIALSOURCE', 146: 'SPECULARMATERIALSOURCE',
    147: 'AMBIENTMATERIALSOURCE', 148: 'EMISSIVEMATERIALSOURCE',
    151: 'VERTEXBLEND', 152: 'CLIPPLANEENABLE', 154: 'POINTSIZE',
    155: 'POINTSIZE_MIN', 156: 'POINTSPRITEENABLE', 157: 'POINTSCALEENABLE',
    158: 'POINTSCALE_A', 159: 'POINTSCALE_B', 160: 'POINTSCALE_C',
    161: 'MULTISAMPLEANTIALIAS', 162: 'MULTISAMPLEMASK', 163: 'PATCHEDGESTYLE',
    166: 'POINTSIZE_MAX', 167: 'INDEXEDVERTEXBLENDENABLE',
    168: 'COLORWRITEENABLE', 170: 'TWEENFACTOR', 171: 'BLENDOP',
    174: 'SCISSORTESTENABLE', 175: 'SLOPESCALEDEPTHBIAS',
    176: 'ANTIALIASEDLINEENABLE', 194: 'SRGBWRITEENABLE',
    206: 'SEPARATEALPHABLENDENABLE', 207: 'SRCBLENDALPHA',
    208: 'DESTBLENDALPHA', 209: 'BLENDOPALPHA',
}

D3DBLEND = {
    1: 'ZERO', 2: 'ONE', 3: 'SRCCOLOR', 4: 'INVSRCCOLOR', 5: 'SRCALPHA',
    6: 'INVSRCALPHA', 7: 'DESTALPHA', 8: 'INVDESTALPHA', 9: 'DESTCOLOR',
    10: 'INVDESTCOLOR', 11: 'SRCALPHASAT', 12: 'BOTHINVSRCALPHA',
    13: 'BLENDFACTOR', 14: 'INVBLENDFACTOR',
}

D3DBLENDOP = {1: 'ADD', 2: 'SUBTRACT', 3: 'REVSUBTRACT', 4: 'MIN', 5: 'MAX'}

D3DCULL = {1: 'NONE', 2: 'CW', 3: 'CCW'}

D3DCMP = {1: 'NEVER', 2: 'LESS', 3: 'EQUAL', 4: 'LESSEQUAL', 5: 'GREATER',
          6: 'NOTEQUAL', 7: 'GREATEREQUAL', 8: 'ALWAYS'}

D3DMCS = {0: 'MATERIAL', 1: 'COLOR1', 2: 'COLOR2'}

BOOL_STATES = {
    'ZENABLE', 'ALPHATESTENABLE', 'ZWRITEENABLE', 'DITHERENABLE',
    'ALPHABLENDENABLE', 'FOGENABLE', 'SPECULARENABLE', 'LIGHTING',
    'COLORVERTEX', 'LOCALVIEWER', 'NORMALIZENORMALS', 'SCISSORTESTENABLE',
    'CLIPPING', 'MULTISAMPLEANTIALIAS', 'INDEXEDVERTEXBLENDENABLE',
    'RANGEFOGENABLE', 'STENCILENABLE', 'SEPARATEALPHABLENDENABLE',
}

VALUE_ENUMS = {
    'SRCBLEND': D3DBLEND, 'DESTBLEND': D3DBLEND,
    'SRCBLENDALPHA': D3DBLEND, 'DESTBLENDALPHA': D3DBLEND,
    'BLENDOP': D3DBLENDOP, 'BLENDOPALPHA': D3DBLENDOP,
    'CULLMODE': D3DCULL, 'ALPHAFUNC': D3DCMP, 'ZFUNC': D3DCMP,
    'DIFFUSEMATERIALSOURCE': D3DMCS, 'SPECULARMATERIALSOURCE': D3DMCS,
    'AMBIENTMATERIALSOURCE': D3DMCS, 'EMISSIVEMATERIALSOURCE': D3DMCS,
}

FOCUS_STATES = ['ALPHABLENDENABLE', 'SRCBLEND', 'DESTBLEND', 'BLENDOP',
                'ALPHATESTENABLE', 'ZENABLE', 'CULLMODE', 'LIGHTING',
                'COLORVERTEX', 'DIFFUSEMATERIALSOURCE']


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


CMP_RE = re.compile(r'^(cmp|test|and|or|xor|shl|shr|sar|inc|dec|add|sub)\b')


def is_setup_instr(text):
    """实参装载区里可以"穿过"的指令：push 寄存器、寄存器/成员搬运、分支、比较。"""
    if text.startswith('push'):
        return True
    if text.startswith('mov'):
        return True
    if BRANCH_RE.match(text):
        return True
    if CMP_RE.match(text):
        return True
    return False


def scan(rows):
    """对每个 `call *%reg`：回退找 vtable 偏移，再回退收立即数 push。"""
    sites = []
    for i, (addr, text) in enumerate(rows):
        m = CALL_RE.match(text)
        if not m:
            continue
        reg = m.group(1)
        vt_off = None
        vt_idx = None
        for j in range(i - 1, max(-1, i - 6), -1):
            vm = VT_MOV_RE.match(rows[j][1])
            if vm and vm.group(3) == reg:
                vt_off = int(vm.group(1), 16)
                vt_idx = j
                break
        if vt_off != VT_OFF:
            continue
        # 往回收立即数 push（允许穿过实参装载区：push 寄存器/寄存器搬运/分支）
        imms = []
        branches = 0
        for j in range(i - 1, max(-1, i - 16), -1):
            t = rows[j][1]
            pm = PUSH_IMM_RE.match(t)
            if pm:
                imms.append((rows[j][0], int(pm.group(1), 16)))
                continue
            if j == vt_idx:
                continue
            if BRANCH_RE.match(t):
                branches += 1
                continue
            if is_setup_instr(t):
                continue
            break
        imms.reverse()
        obj = None
        for j in range(max(0, vt_idx - 4), vt_idx + 1):
            om = OBJ_RE.match(rows[j][1])
            if om:
                obj = '0x%s(%%%s)' % (om.group(1), om.group(2))
        sites.append({'pc': addr, 'vtable_off': vt_off, 'imms': imms,
                      'branches': branches, 'obj': obj, 'text': text})
    return sites


def analyse(sites):
    states = {}
    unresolved = []
    resolved = []
    for s in sites:
        imms = s['imms']
        if len(imms) < 2:
            unresolved.append({'pc': '0x%08x' % s['pc'],
                               'immediates': len(imms),
                               'text': s['text']})
            continue
        state = imms[-1][1]
        values = sorted(set(v for _, v in imms[:-1]))
        resolved.append({'pc': '0x%08x' % s['pc'], 'state': state,
                         'state_name': D3DRS.get(state, 'state_%d' % state),
                         'values': values, 'conditional': len(values) > 1,
                         'obj': s['obj']})
        for v in values:
            states.setdefault(state, set()).add(v)
    out = {}
    for st, vals in states.items():
        name = D3DRS.get(st, 'state_%d' % st)
        vals = sorted(vals)
        out[name] = {
            'state': st,
            'name': name,
            'values': vals,
            'value_names': [value_name(name, v) for v in vals],
            'sites': sorted(r['pc'] for r in resolved if r['state'] == st),
        }
    return out, resolved, unresolved


def value_name(state_name, value):
    if state_name in VALUE_ENUMS:
        return VALUE_ENUMS[state_name].get(value, 'value_%d' % value)
    if state_name in BOOL_STATES:
        return 'TRUE' if value else 'FALSE'
    return None


def build_blend_summary(states, resolved, unresolved):
    src = states.get('SRCBLEND') or {}
    dst = states.get('DESTBLEND') or {}
    enable = states.get('ALPHABLENDENABLE') or {}
    op = states.get('BLENDOP') or {}
    src_names = src.get('value_names') or []
    dst_names = dst.get('value_names') or []
    modes = []
    if 'SRCALPHA' in src_names and 'INVSRCALPHA' in dst_names:
        modes.append({
            'name': 'straight-alpha over',
            'src': 'SRCALPHA', 'dest': 'INVSRCALPHA',
            'formula': 'dst = src.rgb*src.a + dst.rgb*(1-src.a)',
        })
    if 'SRCALPHA' in src_names and 'ONE' in dst_names:
        modes.append({
            'name': 'additive (alpha-add)',
            'src': 'SRCALPHA', 'dest': 'ONE',
            'formula': 'dst = src.rgb*src.a + dst.rgb',
        })
    if 'ONE' in src_names and 'ZERO' in dst_names:
        modes.append({
            'name': 'overwrite (no blend)',
            'src': 'ONE', 'dest': 'ZERO',
            'formula': 'dst = src.rgb',
        })
    return {
        'alpha_blend_enable': enable.get('values'),
        'srcblend': src_names,
        'destblend': dst_names,
        'blendop': op.get('value_names'),
        'blend_modes': modes,
        'conclusion': ('原版一共出现 %d 种混合配置：%s；BLENDOP 从未设置（D3D9 默认 ADD）'
                       % (len(modes), '、'.join(m['name'] for m in modes))
                       if modes else 'insufficient evidence'),
        'coverage': {'resolved_sites': len(resolved),
                     'unresolved_sites': len(unresolved),
                     'unresolved': unresolved},
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows = load_rows(ASM)
    mov_sites = [t for _, t in rows
                 if VT_MOV_RE.match(t)
                 and int(VT_MOV_RE.match(t).group(1), 16) == VT_OFF]
    sites = scan(rows)
    states, resolved, unresolved = analyse(sites)
    blend = build_blend_summary(states, resolved, unresolved)
    result = {
        'note': '从 exe 静态分析抄出的 D3D9 渲染状态常量（A 类事实）；'
                '由 scripts/extract-exe-d3d-states.py 生成。'
                'state/value 枚举名来自 D3D9 SDK 公开常量，不是游戏数据。',
        'asm': os.path.relpath(ASM, ROOT).replace('\\', '/'),
        'vtable_offset': '0x%x' % VT_OFF,
        'mov_0xe4_sites': len(mov_sites),
        'set_render_state_calls': len(resolved),
        'blend_model': blend,
        'render_states': states,
        'call_sites': resolved,
    }

    print('== SetRenderState（vtable 0x%x）：%d 个可解析调用点（asm 里 mov 0x%x(...) 共 %d 处）=='
          % (VT_OFF, len(resolved), VT_OFF, len(mov_sites)))
    for r in resolved:
        print('  %s  %-22s = %-24s%s'
              % (r['pc'], r['state_name'], r['values'],
                 '  (值来自分支)' if r['conditional'] else ''))
    print('== 未解析（实参来自寄存器/更远的分支）%d 处 ==' % len(unresolved))
    for u in unresolved:
        print('  %s  immediates=%d' % (u['pc'], u['immediates']))
    print('== 结论 ==')
    print('  %s' % blend['conclusion'])
    for m in blend['blend_modes']:
        print('  - %-22s %s' % (m['name'], m['formula']))
    print('== 关心的状态 ==')
    for name in FOCUS_STATES:
        e = states.get(name)
        if e is None:
            print('  %-22s (exe 里没设置过 → D3D9 默认值)' % name)
        else:
            print('  %-22s %s' % (name, e['value_names']))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: %s 与提取结果不一致' % OUT_JSON)
            return 1
        print('OK: %s 与提取结果逐字节一致' % OUT_JSON)
        return 0

    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('written %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())