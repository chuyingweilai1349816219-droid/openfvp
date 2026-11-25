#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从脚本里静态提取**标题画面场景**（不跑原版、不截图）。

背景（第 121 轮结论）：`TitleMenu`(141) 只是"进/不进标题菜单"的一个布尔，handler 是
空操作；exe 里也没有 TitleMenu 类。那标题画面是谁画的？答案是**脚本**：脚本里有
`title_bg1_*` / `title_start*` / `title_continue*` … 的 `GraphLoad`，有对图元的
`PrimSetZ`/`PrimSetRS`/`PrimSetDraw`，命中测试也在脚本里做（读鼠标 + `PrimHit`）。

所以"按钮位置分布"这类问题的正确证据源是脚本，不是截图。本脚本把标题场景里
**确定的事实**抽出来冻结：

  1. 标题素材名（`title_*` 字符串常量）与它们被 GraphLoad 进的**槽位号**
     （调用形如 `pushint16 slot; pushstring name; pushint8 kind; pushtrue; call helper`）；
  2. 菜单项素材家族：`title_start` 与 `title_start1..5` 这样的"基名 + 状态变体"；
  3. 标题区域的输入/命中命令调用点（`InputGetCursX/Y`、`InputGetDown`、`PrimHit`）
     以及 `PrimHit` 用到的**图元号**——命中就是"按图元矩形判定"，所以图元号 =
     按钮的身份，矩形由脚本后续的 PrimSet* 设定；
  4. 每个调用点的 pc（便于人工复核）。

产物 `ref/title-scene.json`（只含名字/槽位/图元号等参数，不含游戏数据本体）。

用法：
    python scripts/extract-title-scene.py            # 打印 + 写 ref/title-scene.json
    python scripts/extract-title-scene.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}
OUT_JSON = os.path.join(ROOT, 'ref', 'title-scene.json')

INPUT_CMDS = {
    29: 'InputGetCursX',
    30: 'InputGetCursY',
    31: 'InputGetDown',
    34: 'InputGetState',
    35: 'InputGetUp',
    76: 'PrimHit',
}
PRIM_CMDS = {79: 'PrimSetDraw', 80: 'PrimSetNull', 81: 'PrimSetOP', 85: 'PrimSetSprt',
             88: 'PrimSetUV', 89: 'PrimSetWH', 90: 'PrimSetXY', 91: 'PrimSetZ'}
# 每部作品的标题场景 pc 区域（由第 121/122 轮探查确定：素材装载函数 + 主循环）。
# 两部作品的脚本布局不同，区域必须分开写；写错只会让统计为空，不会伪造数据。
TITLE_REGION = {
    'iroseka': (0x58000, 0x64000),
    'hoshimemo': (0x6f000, 0x76000),
}
FAMILY_RE = re.compile(r'^(title_[a-z0-9]+?)(\d*)$')


def extract(path, argc_table, region):
    with open(path, 'rb') as fp:
        data = fp.read()
    code = fvm_ref.Code(data, 'cp932')
    order = sorted(code.instr)

    strings = []          # 按出现顺序的 title_* 字符串
    loads = []            # {slot, name, helper, pc}
    prim_hit = []         # {prim, pc}
    inputs = []           # {cmd, name, pc}
    prim_ops = []         # {prim, cmd, args, pc}  在标题区域内对图元的操作
    layer_z = []          # {prim, z, pc}  PrimSetZ 的字面量实参（背景层叠放顺序）

    for i, pc in enumerate(order):
        op, name, args = code.instr[pc]
        if name == 'pushstring' and args and str(args[0]).startswith('title_'):
            strings.append({'pc': '0x%08x' % pc, 'name': str(args[0])})
        if name == 'call' and i >= 4:
            window = [code.instr[order[i - k]][1] for k in range(4, 0, -1)]
            vals = [code.instr[order[i - k]][2] for k in range(4, 0, -1)]
            if window == ['pushint16', 'pushstring', 'pushint8', 'pushtrue']:
                nm = str(vals[1][0]) if vals[1] else ''
                if nm.startswith('title_') and vals[0]:
                    loads.append({'slot': vals[0][0], 'name': nm,
                                  'helper': '0x%08x' % args[0],
                                  'pc': '0x%08x' % pc})
        if name == 'syscall' and args and args[0] in INPUT_CMDS:
            if region[0] <= pc < region[1]:
                inputs.append({'cmd': args[0], 'name': INPUT_CMDS[args[0]],
                               'pc': '0x%08x' % pc})
        if name == 'syscall' and args and args[0] == 91 and i >= 2:
            # PrimSetZ(prim, z)：两个字面量可以直接读出来，用来定背景层叠放顺序
            a1 = code.instr[order[i - 2]][2]
            a2 = code.instr[order[i - 1]][2]
            if (a1 and a2 and isinstance(a1[0], int) and isinstance(a2[0], int)
                    and region[0] <= pc < region[1]):
                layer_z.append({'prim': a1[0], 'z': a2[0], 'pc': '0x%08x' % pc})
        if name == 'syscall' and args:
            cid = args[0]
            e = argc_table.get(cid)
            if cid == 76 and e is not None and i >= e['argc']:
                a = [code.instr[order[i - k]][2] for k in range(e['argc'], 0, -1)]
                if a and a[0] and isinstance(a[0][0], int):
                    if region[0] <= pc < region[1]:
                        prim_hit.append({'prim': a[0][0], 'pc': '0x%08x' % pc})
            if cid in PRIM_CMDS and e is not None and i >= e['argc']:
                a = [code.instr[order[i - k]][2] for k in range(e['argc'], 0, -1)]
                vals = [v[0] if v else None for v in a]
                if all(isinstance(v, int) for v in vals) and vals:
                    prim = vals[0]
                    if region[0] <= pc < region[1]:
                        prim_ops.append({'prim': prim, 'cmd': cid,
                                         'name': PRIM_CMDS[cid], 'args': vals,
                                         'pc': '0x%08x' % pc})

    # 素材家族：基名 → 变体列表
    families = {}
    for s in strings:
        m = FAMILY_RE.match(s['name'])
        if not m:
            continue
        base, num = m.group(1), m.group(2)
        families.setdefault(base, set()).add(s['name'])
    fam = {k: sorted(v) for k, v in sorted(families.items())}

    return {
        'hcb_sha256': hashlib.sha256(data).hexdigest(),
        'hcb_size': len(data),
        'instructions': len(order),
        'title_strings': strings,
        'loads': loads,
        'families': fam,
        'title_inputs': inputs,
        'prim_hit': prim_hit,
        'prim_ops_in_title_region': prim_ops,
        'prim_hit_ids': sorted({p['prim'] for p in prim_hit}),
        'layer_z': layer_z,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='all', choices=['all'] + sorted(GAMES))
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)

    with io.open(os.path.join(ROOT, 'ref', 'cmd_table.json'), encoding='utf-8') as fp:
        argc_table = {e['id']: e for e in json.load(fp)}

    names = sorted(GAMES) if opt.game == 'all' else [opt.game]
    result = {
        'note': '标题画面的脚本侧事实（素材名/槽位/图元号/命中命令）；'
                '由 scripts/extract-title-scene.py 生成，不含游戏数据本体',
        'games': {},
    }
    for name in names:
        path = GAMES[name]
        if not os.path.isfile(path):
            print('SKIP: %s 不存在（%s）' % (name, path))
            return 0
        region = TITLE_REGION[name]
        r = extract(path, argc_table, region)
        r['title_region'] = ['0x%08x' % region[0], '0x%08x' % region[1]]
        # 标题区里 PrimHit 的图元号有两组：52/55/58/…/82 是**两部作品都存在**的
        # 消息窗按钮（同号同组，与本作标题无关）；菜单项是另一组。按 id 阈值分开，
        # 阈值 200 的依据是两组之间没有其它 id（见 ref/title-scene.json）。
        r['menu_prim_ids'] = [p for p in r['prim_hit_ids'] if p >= 200]
        result['games'][name] = r
        print('%s  instructions=%d  title_* 字符串=%d  GraphLoad=%d  命中图元=%s'
              % (name, r['instructions'], len(r['title_strings']), len(r['loads']),
                 r['prim_hit_ids']))
        print('   素材家族 %d 个: %s' % (len(r['families']),
                                        ', '.join(sorted(r['families']))[:160]))
        for ld in r['loads'][:8]:
            print('   load slot=%-4d %-16s @%s' % (ld['slot'], ld['name'], ld['pc']))
        print('   输入命令: %s'
              % sorted({x['name'] for x in r['title_inputs']}))
        print('   PrimHit 调用点: %d 个，图元号 %s'
              % (len(r['prim_hit']), r['prim_hit_ids'][:12]))

    text = json.dumps(result, ensure_ascii=False, indent=1, sort_keys=True) + '\n'
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        with io.open(OUT_JSON, encoding='utf-8') as fp:
            old = fp.read()
        if old != text:
            print('FAIL: 提取结果与 ref/title-scene.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/title-scene.json 一致')
        return 0
    with io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())