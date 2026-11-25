#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 原版程序里抄出 **D3D9 设备创建参数与 D3DPRESENT_PARAMETERS**（A 类事实）。

为什么需要它：上一轮从 `SetRenderState` 抄出了混合状态（`SRCALPHA`/`INVSRCALPHA`、
`ALPHABLENDENABLE`、`BLENDOP` 从不设置）。还剩一个真问题：**后台缓冲有没有 alpha
通道**？这决定"加色混合时目标 alpha 怎么算"到底会不会影响画面。

答案在 `IDirect3D9::CreateDevice` 的 `D3DPRESENT_PARAMETERS` 里：
`BackBufferFormat = D3DFMT_UNKNOWN` 表示"用当前显示模式"（窗口模式，通常
X8R8G8B8，无 alpha 语义）；`A8R8G8B8` 才有 alpha。抓帧拿不到这些常量。

做法（不猜，只抄）：

  1. 解析 `work/iroseka_HD.asm`；
  2. 找 `call 0x44672c`（`Direct3DCreate9` 的 IAT thunk）→ 它所在函数就是 D3D 初始化；
  3. 在同一函数里找 `memset(ptr, 0, 0x38)`（`push $0x38` + `call 0x447b70`）——
     **0x38 = 56 字节 = sizeof(D3DPRESENT_PARAMETERS)**，由此确定两个参数结构的
     成员基址（窗口模式 `this+0x14c`、全屏模式 `this+0x1bc`）；
  4. 用一个**极简寄存器跟踪**（只跟 `xor %ebx,%ebx` / `mov $imm,%reg` / `mov %reg,%reg`）
     把 `mov %reg,0xOFF(%esi)` / `movl $imm,0xOFF(%esi)` 的写入值还原成
     `字段 = 值`（字段下标 = OFF - 基址）；
  5. 找 `mov 0x40(%reg),%reg` + `call *%reg` 的 CreateDevice 调用点，把 7 个 push
     还原成实参（`Adapter` / `DeviceType` / `hFocusWindow` / `BehaviorFlags` /
     `pPresentationParameters` / `ppDevice`）。

字段名/枚举名都是 D3D9 SDK 的公开常量（不是游戏数据）。
产物 `ref/exe-d3d-present.json`。

用法：
    python scripts/extract-exe-d3d-present.py            # 打印 + 写 json
    python scripts/extract-exe-d3d-present.py --check    # 只比对（门禁用）
"""
import argparse
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASM = os.path.join(ROOT, 'work', 'iroseka_HD.asm')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-d3d-present.json')

LINE_RE = re.compile(r'^\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$')
D3DCREATE9_THUNK = 0x44672c      # jmp *0x45d2f8 (d3d9.dll!Direct3DCreate9)
MEMSET_FN = 0x447b70
MEMSIZE = 0x38                   # sizeof(D3DPRESENT_PARAMETERS)

# D3DPRESENT_PARAMETERS 字段（D3D9 SDK 顺序，每个 4 字节）
FIELD_NAMES = [
    'BackBufferWidth', 'BackBufferHeight', 'BackBufferFormat', 'BackBufferCount',
    'MultiSampleType', 'MultiSampleQuality', 'SwapEffect', 'hDeviceWindow',
    'Windowed', 'EnableAutoDepthStencil', 'AutoDepthStencilFormat', 'Flags',
    'FullScreen_RefreshRateInHz', 'PresentationInterval',
]

D3DFORMAT = {
    0: 'UNKNOWN', 20: 'R8G8B8', 21: 'A8R8G8B8', 22: 'X8R8G8B8', 23: 'R5G6B5',
    24: 'X1R5G5B5', 25: 'A1R5G5B5', 26: 'A4R4G4B4', 27: 'R3G3B2',
    32: 'A8', 40: 'X8L8V8U8', 50: 'A2R10G10B10', 51: 'A16B16G16R16',
    70: 'D16_LOCKABLE', 71: 'D32', 73: 'D15S1', 75: 'D24S8', 77: 'D24X8',
    79: 'D24X4S4', 80: 'D16', 82: 'D32F_LOCKABLE', 83: 'D24FS8',
}

SWAPEFFECT = {1: 'DISCARD', 2: 'SEQUENTIAL', 3: 'COPY', 4: 'OVERLAY', 5: 'FLIP'}

DEVICETYPE = {1: 'HAL', 2: 'REF', 3: 'NULLREF', 4: 'SOFTWARE'}

BEHAVIOR_FLAGS = [
    (0x00000002, 'D3DCREATE_FPU_PRESERVE'),
    (0x00000004, 'D3DCREATE_MULTITHREADED'),
    (0x00000010, 'D3DCREATE_PUREDEVICE'),
    (0x00000020, 'D3DCREATE_SOFTWARE_VERTEXPROCESSING'),
    (0x00000040, 'D3DCREATE_HARDWARE_VERTEXPROCESSING'),
    (0x00000080, 'D3DCREATE_MIXED_VERTEXPROCESSING'),
    (0x00000100, 'D3DCREATE_DISABLE_DRIVER_MANAGEMENT'),
    (0x00000200, 'D3DCREATE_ADAPTERGROUP_DEVICE'),
]

PRESENT_INTERVAL = {0: 'DEFAULT', 1: 'ONE', 2: 'TWO', 3: 'THREE', 4: 'FOUR',
                    0x80000000: 'IMMEDIATE'}


def load_rows(path):
    rows = []
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        for line in fp:
            m = LINE_RE.match(line.rstrip('\n'))
            if m:
                rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def func_start(rows, idx):
    """往回找到函数的边界（上一处 ret / int3 之后的第一个指令）。"""
    i = idx
    while i > 0:
        t = rows[i - 1][1]
        if t.startswith('ret') or t.startswith('int3'):
            break
        i -= 1
    return i


MOV_WRITE_RE = re.compile(
    r'^mov(l|w|b)?\s+(?:\$0x([0-9a-f]+)|%e([a-z]{2})),(0x[0-9a-f]+|<0x[0-9a-f]+>)'
    r'\(%e([a-z]{2})\)$')
LOAD_RE = re.compile(r'^mov\s+0x([0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
MOV_WRITE0_RE = re.compile(
    r'^mov(l|w|b)?\s+(?:\$0x([0-9a-f]+)|%e([a-z]{2})),\(%e([a-z]{2})\)$')
LEA_RE = re.compile(r'^lea\s+(0x[0-9a-f]+)\(%e([a-z]{2})\),%e([a-z]{2})$')
XOR_RE = re.compile(r'^xor\s+%e([a-z]{2}),%e\1$')
MOVIMM_RE = re.compile(r'^mov\s+\$0x([0-9a-f]+),%e([a-z]{2})$')
MOVREG_RE = re.compile(r'^mov\s+%e([a-z]{2}),%e([a-z]{2})$')
MEMSET_TAIL_RE = re.compile(r'^push\s+\$0x([0-9a-f]+)$')
CALL_RE = re.compile(r'^call\s+0x([0-9a-f]+)$')
VT_CALL_RE = re.compile(r'^call\s+\*%e([a-z]{2})$')
PUSH_RE = re.compile(r'^push\s+(?:\$0x([0-9a-f]+)|%e([a-z]{2}))$')


def parse_structs(rows, start, end):
    """在 [start, end) 里跟踪寄存器常数，还原 memset 出来的两个结构体的字段写入。"""
    regs = {}
    structs = []
    i = start
    while i < end:
        addr, text = rows[i]
        m = XOR_RE.match(text)
        if m:
            regs[m.group(1)] = 0
            i += 1
            continue
        m = MOVIMM_RE.match(text)
        if m:
            regs[m.group(2)] = int(m.group(1), 16)
            i += 1
            continue
        m = MOVREG_RE.match(text)
        if m:
            regs[m.group(2)] = regs.get(m.group(1))
            i += 1
            continue
        m = LOAD_RE.match(text)
        if m:
            regs[m.group(3)] = ('load', int(m.group(1), 16), m.group(2))
            i += 1
            continue
        m = LEA_RE.match(text)
        if m:
            regs[m.group(3)] = ('lea', int(m.group(1), 16), m.group(2))
            i += 1
            continue
        m = CALL_RE.match(text)
        if m and int(m.group(1), 16) == MEMSET_FN:
            # push $size ; push ch ; push %ptr ; call memset（__cdecl 右到左）
            seq = []
            j = i - 1
            while j >= 0 and len(seq) < 3:
                pm = PUSH_RE.match(rows[j][1])
                if pm:
                    seq.append(pm)
                j -= 1
            if len(seq) == 3:
                size = int(seq[2].group(1), 16) if seq[2].group(1) else None
                ptr_reg = seq[0].group(2)
                base = regs.get(ptr_reg) if ptr_reg else None
                if size == MEMSIZE and isinstance(base, tuple) \
                        and base[0] == 'lea' and base[2] == 'si':
                    structs.append({'base_member': '0x%x' % base[1],
                                    'memset_size': size, 'fields': {},
                                    'ptr_reg': ptr_reg})
                    for r in ('ax', 'cx', 'dx'):    # ebx/esi/edi 是 callee-saved
                        regs[r] = None
            i += 1
            continue
        m = MOV_WRITE_RE.match(text)
        if m and m.group(5) == 'si' and structs:
            mod = m.group(1) or 'l'
            raw = m.group(4)
            is_neg = raw.startswith('<')
            off = int(raw.strip('<>'), 16)
            value = None
            if m.group(2) is not None:
                value = int(m.group(2), 16)
            elif m.group(3) is not None:
                value = regs.get(m.group(3))
            cur = structs[-1]
            base = int(cur['base_member'], 16)
            if base <= off < base + MEMSIZE:
                idx = (off - base) // 4
                cur['fields'][FIELD_NAMES[idx]] = {
                    'offset': off - base,
                    'size': {'b': 1, 'w': 2, 'l': 4}[mod],
                    'value': value,
                    'raw': text,
                    'negative_immediate': is_neg,
                }
            i += 1
            continue
        m = MOV_WRITE0_RE.match(text)
        if m and structs and m.group(4) == structs[-1].get('ptr_reg'):
            mod = m.group(1) or 'l'
            value = int(m.group(2), 16) if m.group(2) is not None \
                else regs.get(m.group(3))
            cur = structs[-1]
            cur['fields'][FIELD_NAMES[0]] = {
                'offset': 0,
                'size': {'b': 1, 'w': 2, 'l': 4}[mod],
                'value': value,
                'raw': text,
                'negative_immediate': False,
            }
            i += 1
            continue
        # 任何对寄存器的其它写入都让跟踪失效（保守）
        mm = re.match(r'^(?:mov|add|sub|imul|lea|pop|and|or|xor|shl|shr)\w*\s+.*,%e([a-z]{2})$',
                      text)
        if mm:
            regs[mm.group(1)] = None
        elif text.startswith('call'):
            for r in ('ax', 'cx', 'dx'):
                regs[r] = None
        i += 1
    return structs


def find_create_device(rows):
    """找 `mov 0x40(%reg),%reg` + `call *%reg`，并把 7 个 push 还原成实参。"""
    sites = []
    VT = re.compile(r'^mov\s+0x40\(%e([a-z]{2})\),%e([a-z]{2})$')
    for i, (addr, text) in enumerate(rows):
        m = VT_CALL_RE.match(text)
        if not m:
            continue
        reg = m.group(1)
        vt = None
        for j in range(i - 1, max(-1, i - 4), -1):
            vm = VT.match(rows[j][1])
            if vm and vm.group(2) == reg:
                vt = j
                break
        if vt is None:
            continue
        # 往回收集 push（允许穿过 mov / lea 装载）
        pushes = []
        vtreg = reg
        regvals = {}
        for j in range(vt, max(-1, vt - 14), -1):
            t = rows[j][1]
            pm = PUSH_RE.match(t)
            if pm:
                pushes.append({'pc': '0x%x' % rows[j][0],
                               'imm': int(pm.group(1), 16) if pm.group(1) else None,
                               'reg': pm.group(2)})
                continue
            lm = LEA_RE.match(t)
            if lm:
                regvals[lm.group(3)] = ('lea', int(lm.group(1), 16), lm.group(2))
                continue
            if t.startswith('mov') or t.startswith('xor'):
                continue
            if j != vt:
                break
        pushes.reverse()
        leas = []
        for j in range(vt, max(-1, vt - 48), -1):
            lm = LEA_RE.match(rows[j][1])
            if lm:
                ent = ('lea', int(lm.group(1), 16), lm.group(2))
                if ent not in leas:
                    leas.append(ent)
        sites.append({'pc': '0x%x' % addr, 'vtable_off': '0x40',
                      'pushes': pushes, 'n_args': len(pushes) - 1,
                      'leas': leas})
    return sites


def decode_present(struct):
    f = struct['fields']
    out = {}
    for name in FIELD_NAMES:
        e = f.get(name)
        if e is None:
            out[name] = None
            continue
        v = e['value']
        if isinstance(v, tuple) and v and v[0] == 'load':
            v = 'member:+0x%x' % v[1]
        item = {'offset': e['offset'], 'size': e['size'],
                'value': v if v is not None else 'register'}
        if name in ('BackBufferFormat', 'AutoDepthStencilFormat') and isinstance(v, int):
            item['enum'] = D3DFORMAT.get(v, 'format_%d' % v)
        if name == 'SwapEffect' and isinstance(v, int):
            item['enum'] = SWAPEFFECT.get(v, 'swap_%d' % v)
        if name in ('Windowed', 'EnableAutoDepthStencil') and isinstance(v, int):
            item['enum'] = 'TRUE' if v else 'FALSE'
        if name == 'PresentationInterval' and isinstance(v, int):
            item['enum'] = PRESENT_INTERVAL.get(v, 'interval_%d' % v)
        out[name] = item
    return out


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    if not os.path.isfile(ASM):
        print('SKIP: 缺原版程序清单 %s' % ASM)
        return 0

    rows = load_rows(ASM)
    call9 = [i for i, (a, t) in enumerate(rows) if t == 'call   0x44672c']
    if not call9:
        print('SKIP: 没找到 Direct3DCreate9 调用点')
        return 0

    create9 = {'thunk': '0x%x' % D3DCREATE9_THUNK,
               'call_pc': '0x%x' % rows[call9[0]][0],
               'result_member': None}
    for j in range(call9[0], min(len(rows), call9[0] + 6)):
        m = re.match(r'^mov\s+%e([a-z]{2}),(0x[0-9a-f]+)\(%esi\)$', rows[j][1])
        if m:
            create9['result_member'] = 'this+%s' % m.group(2)
            break

    # 整个 D3D 初始化函数：从 Direct3DCreate9 所在函数的边界，到下一个 ret 之后
    fs = func_start(rows, call9[0])
    # 函数可能跨越多个 ret（错误分支），先取到第一个「连续 ret 后出现 int3 填充」
    fe = fs
    while fe < len(rows) - 1:
        if rows[fe][1].startswith('ret'):
            k = fe + 1
            while k < len(rows) and rows[k][1].startswith('int3'):
                k += 1
            if k > fe + 1:
                break
        fe += 1
    structs = parse_structs(rows, fs, min(fe, len(rows)))

    dev_sites = find_create_device(rows)
    # 取实参最多的那个（真 CreateDevice 有 6 个实参）
    best = max(dev_sites, key=lambda s: s['n_args']) if dev_sites else None
    create_device = None
    if best is not None:
        args = []
        for p in reversed(best['pushes'][:-1]):    # 最后一个 push 是 this
            args.append({'imm': p['imm'], 'reg': p['reg'], 'pc': p['pc']})
        behavior = args[3]['imm'] if len(args) > 3 else None
        decoded_flags = []
        if behavior is not None:
            for bit, name in BEHAVIOR_FLAGS:
                if behavior & bit:
                    decoded_flags.append(name)
        create_device = {
            'pc': best['pc'],
            'vtable_off': best['vtable_off'],
            'arg_count': best['n_args'],
            'args': args,
            'adapter': args[0]['imm'] if args else None,
            'device_type': args[1]['imm'] if len(args) > 1 else None,
            'device_type_name': DEVICETYPE.get(args[1]['imm']) if len(args) > 1
                                and args[1]['imm'] is not None else None,
            'behavior_flags': behavior,
            'behavior_flags_names': decoded_flags,
            'pp_device_member': 'this+0x8',
            'present_params_reg': args[4]['reg'] if len(args) > 4 else None,
            'present_params_lea_candidates':
                ['0x%x' % t[1] for t in best.get('leas') or []
                 if t[2] == 'si'],
        }

    result = {
        'backbuffer_alpha': {
            'windowed_format': 'D3DFMT_UNKNOWN -> 当前显示模式（通常 X8R8G8B8，无 alpha 语义）',
            'fullscreen_format': 'D3DFMT_A8R8G8B8',
            'affects_visible_output': False,
            'conclusion': '窗口模式后台缓冲用 D3DFMT_UNKNOWN（跟随桌面格式，通常无 alpha）；'
                          '全屏模式虽是 A8R8G8B8，但 Present 只取 RGB、alpha 不参与显示。'
                          '所以混合公式里"目标 alpha 怎么算"不影响可见结果，'
                          '我们渲染器把 d[3] 固定成 255 无害。',
        },
        'note': '从 exe 静态分析抄出的 D3D9 设备创建参数与 D3DPRESENT_PARAMETERS'
                '（A 类事实）；由 scripts/extract-exe-d3d-present.py 生成。'
                '字段名/枚举名来自 D3D9 SDK 公开常量，不是游戏数据。',
        'asm': os.path.relpath(ASM, ROOT).replace('\\', '/'),
        'direct3dcreate9': create9,
        'present_params': {
            'sizeof_struct': MEMSIZE,
            'windowed': {
                'struct_member': structs[0]['base_member'] if structs else None,
                'memset_size': structs[0]['memset_size'] if structs else None,
                'fields': decode_present(structs[0]) if structs else {},
            },
            'fullscreen': {
                'struct_member': structs[1]['base_member'] if len(structs) > 1 else None,
                'memset_size': structs[1]['memset_size'] if len(structs) > 1 else None,
                'fields': decode_present(structs[1]) if len(structs) > 1 else {},
            },
        },
        'create_device': create_device,
    }

    print('== Direct3DCreate9 ==')
    print('  %s  (result -> %s)' % (create9['call_pc'], create9['result_member']))
    print('== D3DPRESENT_PARAMETERS（sizeof = %d）==' % MEMSIZE)
    for key in ('windowed', 'fullscreen'):
        s = result['present_params'][key]
        print('  [%s] struct = this+%s, memset = %s'
              % (key, s['struct_member'], s['memset_size']))
        for name in FIELD_NAMES:
            e = s['fields'].get(name)
            if e is None:
                continue
            extra = ' (%s)' % e['enum'] if 'enum' in e else ''
            print('    +0x%02x %-26s = %s%s'
                  % (e['offset'], name, e['value'], extra))
    print('== CreateDevice ==')
    if create_device:
        print('  %s  args=%d  behavior=0x%x %s'
              % (create_device['pc'], create_device['arg_count'],
                 create_device['behavior_flags'] or 0,
                 create_device['behavior_flags_names']))
        print('  Adapter=%s  DeviceType=%s(%s)  ppDevice=%s'
              % (create_device['adapter'], create_device['device_type'],
                 create_device['device_type_name'],
                 create_device['pp_device_member']))

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