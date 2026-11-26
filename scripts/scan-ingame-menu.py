#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-ingame-menu.py -- 从 HCB 与 HZC1 头静态抽出「系统菜单 + 确认框」的结构与几何。

为什么要这个：FVP 的 148 条命令表里**没有任何 Menu 系命令**（只有 MenuSmessSkip(41)
与 TitleMenu(141)），exe 里也没有菜单文案字符串。也就是说《五彩斑斓的世界 HD》的
系统菜单不是外壳画的，而是脚本自己画：

    0x54C68  系统菜单入口（输入循环、命中、淡入淡出），由脚本主循环在
             InputGetState/InputGetDown 的 bit1 触发（0x5B57D..0x5B596）
    0x54520  载入 8 个环形菜单项 × 3 态（sys_*_1 / _2 / _M）+ 侧窗 sys_window_*
    0x577C8  按动作号打开确认框（menu_*_mes 提示图 + yes/no 按钮 + 光标）
    0x57383  载入确认框素材

**几何不在脚本里、也不在 exe 里**：每个图元的屏幕位置来自 HZC1 头 `0x18/0x1A`
（x/y）与 `0x14/0x16`（w/h）；脚本始终用 `PrimSetSprt(..., true, true)` 保留这两个值。
所以本脚本读 `extract/graph/`（bin_archiver 解包产物）的 44 字节头来取几何，
只输出数字，不含任何图像数据。

用法：
    python scripts/scan-ingame-menu.py                     # 人读摘要
    python scripts/scan-ingame-menu.py --json out/x.json
    python scripts/scan-ingame-menu.py --extract <dir>      # 指定 HZC1 解包目录
"""
import argparse
import bisect
import hashlib
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads',
                            '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads',
                              'hoshimemo_installed', 'Hoshimemo_HD.hcb'),
}

SYS_REGION = (0x54480, 0x54A00)     # 环形菜单 + 侧窗
DLG_REGION = (0x57300, 0x57800)     # 确认框
WRAP_LOAD = 0x2354B
WRAP_GROUP = 0x235E8
ANCHOR_HELPER = 0x35AB7
SHOW_HELPER = 0x23804
SPRT_HELPER = 0x35BCC
SYS_ENTRY = 0x54C68
DLG_ENTRY = 0x577C8
ITEM_PRIM = 94
BG_PRIM = 93
CURSOR_PRIM = 137
CONFIRM_PRIMS = (95, 96, 97, 98)
ACTIONS = {1: 'undo', 2: 'title', 3: 'exit', 4: 'qload',
           5: 'overwrite', 6: 'delete', 7: 'init'}
ACTION_ZH = {'undo': '前回の選択肢へ戻る', 'title': 'タイトルへ戻る',
             'exit': 'ゲームを終了', 'qload': 'クイックロード',
             'overwrite': '記録を上書き', 'delete': '記録を削除',
             'init': '設定を初期化'}


def read_rect(extract, name):
    """从 extract/graph/<idx>_<name> 的 44 字节头取 [x, y, w, h]。"""
    if not extract or not os.path.isdir(extract):
        return None
    for fn in os.listdir(extract):
        base = fn.split('_', 1)[1] if '_' in fn else fn
        if base != name:
            continue
        with open(os.path.join(extract, fn), 'rb') as f:
            h = f.read(0x2C)
        if h[:4] != b'hzc1' or h[12:16] != b'NVSG':
            return None
        w, ht = struct.unpack_from('<H', h, 0x14)[0], struct.unpack_from('<H', h, 0x16)[0]
        x, y = struct.unpack_from('<H', h, 0x18)[0], struct.unpack_from('<H', h, 0x1A)[0]
        return [x, y, w, ht]
    return None


def _window(code, lo, hi):
    return [(p,) + code.instr[p] for p in sorted(code.instr) if lo <= p < hi]


def _call_after(seq, i, target, span=6):
    for j in range(i + 1, min(i + span, len(seq))):
        if seq[j][2] == 'call' and seq[j][3] and seq[j][3][0] == target:
            return seq[j][0]
        if seq[j][2] in ('ret', 'ret2'):
            break
    return None


def scan_system_menu(code, extract):
    seq = _window(code, *SYS_REGION)
    loads = []
    for i, (pc, _op, name, args) in enumerate(seq):
        if name != 'pushstring' or not args or not args[0].startswith('sys_'):
            continue
        if _call_after(seq, i, WRAP_LOAD) is None:
            continue
        prim = None
        for j in range(i - 1, max(-1, i - 3), -1):
            if seq[j][2] in ('pushint16', 'pushint8'):
                prim = seq[j][3][0]
                break
        loads.append(dict(prim=prim, asset=args[0], load_pc='0x%05x' % pc))

    items, order = [], 0
    by_base = {}
    for r in loads:
        a = r['asset']
        if not a.startswith('sys_') or a.startswith('sys_window') or a.startswith('sys_cap'):
            continue
        if a.endswith('_1'):
            order += 1
            by_base[a[:-2]] = dict(order=order, action=a[4:-2], prims={}, assets={})
        base = a[:-2]
        if base not in by_base:
            continue
        slot = {'_1': 'normal', '_2': 'hover', '_M': 'mask'}[a[-2:]]
        by_base[base]['prims'][slot] = r['prim']
        by_base[base]['assets'][slot] = a
    for base, it in by_base.items():
        it['rect'] = read_rect(extract, it['assets'].get('normal', ''))
        it['mask_rect'] = read_rect(extract, it['assets'].get('mask', ''))
        it['hover_rect'] = read_rect(extract, it['assets'].get('hover', ''))
        items.append(it)
    items.sort(key=lambda r: r['order'])

    window = []
    for r in loads:
        if not r['asset'].startswith('sys_window'):
            continue
        window.append(dict(asset=r['asset'], prim=r['prim'],
                           rect=read_rect(extract, r['asset'])))
    window.sort(key=lambda r: r['prim'])

    # 打开路径：主循环里的 InputGetState/InputGetDown bit1 -> call SYS_ENTRY
    openers = []
    allpcs = sorted(code.instr)
    for pc in allpcs:
        op, name, args = code.instr[pc]
        if name != 'call' or not args or args[0] != SYS_ENTRY:
            continue
        back = []
        i = allpcs.index(pc)
        for d in range(1, 8):
            if i - d < 0:
                break
            q = allpcs[i - d]
            back.append('%s%s' % (code.instr[q][1], code.instr[q][2]))
        openers.append(dict(pc='0x%05x' % pc, before=list(reversed(back))))
    return items, window, openers


def scan_dialog(code, extract):
    seq = _window(code, *DLG_REGION)

    def at(i):
        return seq[i] if 0 <= i < len(seq) else (None, None, None, [])

    prompts, loads, cur = [], [], None
    for i, (pc, _op, name, args) in enumerate(seq):
        _p1, _o1, n1, a1 = at(i + 1)
        _p2, _o2, n2, a2 = at(i + 2)
        if name == 'pushstack' and args == [254] and n1 == 'pushint8' and n2 == 'eq':
            cur = a1[0]
        if name not in ('pushint8', 'pushint16') or n1 != 'pushstring' or not a1:
            continue
        if not a1[0].startswith('menu'):
            continue
        if _call_after(seq, i + 1, WRAP_LOAD) is None:
            continue
        parent = enable = None
        for j in range(i + 2, min(i + 16, len(seq))):
            q = seq[j]
            if q[2] == 'pushint16' and q[3] and q[3][0] in (795, 796) and parent is None:
                parent = q[3][0]
            if q[2] == 'pushglobal' and enable is None:
                enable = q[3][0]
        rec = dict(prim=args[0], image=a1[0], load_pc='0x%05x' % pc,
                   parent=parent, enable_global=enable,
                   rect=read_rect(extract, a1[0]))
        loads.append(rec)
        if args[0] == ITEM_PRIM:
            rec['action'] = ACTIONS.get(cur)
            rec['action_zh'] = ACTION_ZH.get(rec['action'])
            prompts.append(rec)
    prompts.sort(key=lambda r: list(ACTIONS).index(next(
        k for k, v in ACTIONS.items() if v == r['action'])))

    strings = [a[0] for _, _, n, a in seq if n == 'pushstring' and a]
    floats = ['0x%08x' % f for f in
              sorted({a[0] for _, _, n, a in _window(code, ANCHOR_HELPER, ANCHOR_HELPER + 0x60)
                      if n == 'pushfloat'})]

    allpcs = sorted(code.instr)
    pos = {p: i for i, p in enumerate(allpcs)}
    fn_starts = [p for p in allpcs if code.instr[p][1] == 'initstack']

    def step(pc, n):
        i = pos.get(pc)
        if i is None or not (0 <= i + n < len(allpcs)):
            return (None, None, None, [])
        q = allpcs[i + n]
        return (q,) + code.instr[q]

    def fn_of(pc):
        k = bisect.bisect_right(fn_starts, pc) - 1
        return fn_starts[k] if k >= 0 else None

    by_global = {p['enable_global']: p for p in prompts if p.get('enable_global')}
    writes, enable_map, opt = [], [], None
    for pc in allpcs:
        _op, name, args = code.instr[pc]
        _p1, _o1, n1, a1 = step(pc, 1)
        _p2, _o2, n2, a2 = step(pc, 2)
        if name == 'pushstack' and args == [253] and n1 == 'pushint8' and n2 == 'eq':
            opt = a1[0]
        if name != 'popglobal' or not args or args[0] not in by_global:
            continue
        g = args[0]
        _pm, _om, pn, pa = step(pc, -1)
        if pn == 'pushfalse':
            value = 'false'
        elif pn == 'pushtrue':
            value = 'true'
        elif pn == 'pushglobal' and pa:
            value = 'global:%d' % pa[0]
        elif pn == 'pushstack':
            value = 'arg'
        else:
            value = pn or '?'
        fnpc = fn_of(pc)
        writes.append(dict(pc='0x%05x' % pc, global_=g,
                           fn=('0x%05x' % fnpc) if fnpc is not None else '-', value=value))
        if pn == 'pushstack' and opt is not None:
            it = by_global[g]
            enable_map.append(dict(option_id=opt, global_=g, action=it['action'],
                                   image=it['image'], write_pc='0x%05x' % pc))
    enable_map.sort(key=lambda r: (r['option_id'] is None, r['option_id']))
    copy_by_global = {w['global_']: w['value'] for w in writes
                      if w['value'].startswith('global:')}
    for m in enable_map:
        src = copy_by_global.get(m['global_'])
        m['config_global'] = int(src.split(':')[1]) if src else None

    # 每个确认动作被哪些代码路径触发
    sites = []
    for pc in allpcs:
        op, name, args = code.instr[pc]
        if name != 'call' or not args or args[0] != DLG_ENTRY:
            continue
        idx = None
        i = pos[pc]
        for d in range(1, 6):
            q = allpcs[i - d]
            if code.instr[q][1] in ('pushint8', 'pushint16'):
                idx = code.instr[q][2][0]
                break
        sites.append(dict(pc='0x%05x' % pc, action=ACTIONS.get(idx),
                          fn=('0x%05x' % fn_of(pc))))
    for p in prompts:
        p['call_sites'] = [s['pc'] for s in sites if s['action'] == p['action']]

    background = next((r for r in loads if r['prim'] == BG_PRIM), None)
    cursor = next((r for r in loads if r['prim'] == CURSOR_PRIM), None)
    return dict(prompts=prompts, loads=loads, strings=strings, floats=floats,
                writes=writes, enable_map=enable_map,
                background=background, cursor=cursor,
                confirms=[r for r in loads if r['prim'] in CONFIRM_PRIMS],
                sites=sites)


def scan_inventory(code, extract):
    """所有 UI 素材（fc_/slf_/sys_/menu_）的载入点：图元号 + 名字 + HZC1 头矩形。"""
    allpcs = sorted(code.instr)
    pos = {p: i for i, p in enumerate(allpcs)}
    rows = []
    for p in allpcs:
        op, name, args = code.instr[p]
        if name != 'pushstring' or not args:
            continue
        asset = args[0]
        if not asset.startswith(('fc_', 'slf_', 'sys_', 'menu_')):
            continue
        wrapped = False
        for d in range(1, 6):
            if pos[p] + d >= len(allpcs):
                break
            q = code.instr[allpcs[pos[p] + d]]
            if q[1] == 'call':
                wrapped = bool(q[2] and q[2][0] == WRAP_LOAD)
                break
        prim = None
        if pos[p] > 0:
            prev = code.instr[allpcs[pos[p] - 1]]
            if prev[1] in ('pushint8', 'pushint16'):
                prim = prev[2][0]
        rows.append(dict(asset=asset, family=asset.split('_')[0], prim=prim,
                         load_pc='0x%05x' % p, via_wrapper=wrapped,
                         rect=read_rect(extract, asset)))
    return rows


def scan(path, extract):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    sys_items, sys_window, openers = scan_system_menu(code, extract)
    dlg = scan_dialog(code, extract)
    dlg_seq = _window(code, *DLG_REGION)
    inventory = scan_inventory(code, extract)
    return dict(
        hcb_size=len(data), hcb_sha256=hashlib.sha256(data).hexdigest(),
        inventory=inventory,
        system=dict(region=['0x%05x' % SYS_REGION[0], '0x%05x' % SYS_REGION[1]],
                    entry='0x%05x' % SYS_ENTRY, items=sys_items,
                    window=sys_window, openers=openers,
                    asset_loads=sum(1 for _, _, n, a in
                                    _window(code, *SYS_REGION)
                                    if n == 'call' and a and a[0] == WRAP_LOAD)),
        dialog=dict(region=['0x%05x' % DLG_REGION[0], '0x%05x' % DLG_REGION[1]],
                    entry='0x%05x' % DLG_ENTRY, instructions=len(dlg_seq),
                    prompts=dlg['prompts'], confirms=dlg['confirms'],
                    background=dlg['background'], cursor=dlg['cursor'],
                    strings=dlg['strings'], floats=dlg['floats'],
                    writes=dlg['writes'], enable_map=dlg['enable_map'],
                    sites=dlg['sites']),
    )


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='iroseka', choices=sorted(GAMES))
    ap.add_argument('--json', default='')
    ap.add_argument('--extract', default=os.path.join(ROOT, 'extract', 'graph'),
                    help='HZC1 解包目录（缺省 extract/graph；不存在则不含几何）')
    a = ap.parse_args(argv)
    path = GAMES[a.game]
    if not os.path.isfile(path):
        print('SKIP: %s not found' % path)
        return 0
    facts = scan(path, a.extract)
    ext = a.extract if os.path.isdir(a.extract) else None

    print('%s: hcb %d B sha256 %s' % (a.game, facts['hcb_size'],
                                      facts['hcb_sha256'][:16]))
    print('system menu %s..%s entry %s, %d asset load(s)'
          % (facts['system']['region'][0], facts['system']['region'][1],
             facts['system']['entry'], facts['system']['asset_loads']))
    for it in facts['system']['items']:
        print('  %d %-9s prims %-3s/%-3s/%-3s  %-16s rect %s  mask %s'
              % (it['order'], it['action'], it['prims'].get('normal'),
                 it['prims'].get('hover'), it['prims'].get('mask'),
                 it['assets'].get('normal'), it['rect'], it['mask_rect']))
    print('  window: %s' % ', '.join('%s@%s' % (w['asset'], w['prim'])
                                     for w in facts['system']['window']))
    print('  openers: %s' % ', '.join(o['pc'] for o in facts['system']['openers']))
    print('confirm dialog %s..%s entry %s, %d instruction(s)'
          % (facts['dialog']['region'][0], facts['dialog']['region'][1],
             facts['dialog']['entry'], facts['dialog']['instructions']))
    for p in facts['dialog']['prompts']:
        print('  %-9s %-16s prim %d parent %s global %s rect %s  sites %s'
              % (p['action'], p['image'], p['prim'], p['parent'],
                 p['enable_global'], p['rect'], ','.join(p['call_sites'])))
    print('  yes/no: %s' % ', '.join('%s@%d' % (c['image'], c['prim'])
                                     for c in facts['dialog']['confirms']))
    print('  geometry: %s' % ('extract/graph 头' if ext else '未提供 --extract，省略'))
    fams = {}
    for r in facts['inventory']:
        fams.setdefault(r['family'], []).append(r)
    print('UI 素材清单: %s' % ', '.join('%s %d' % (k, len(v))
                                       for k, v in sorted(fams.items())))
    for fam in sorted(fams):
        for r in fams[fam]:
            if fam in ('fc', 'slf'):
                print('  %-4s %-16s prim %-4s %s  rect %s'
                      % (fam, r['asset'], r['prim'],
                         'wrap' if r['via_wrapper'] else 'direct', r['rect']))
    if a.json:
        payload = dict(
            schema='fvp-ingame-menu/2', game=a.game,
            source=dict(file=os.path.basename(path), size=facts['hcb_size'],
                        sha256=facts['hcb_sha256'],
                        method='HCB 静态解码 + HZC1 44 字节头；只读结构/坐标，不含图像'),
            claim=('系统菜单与确认框都由脚本用图片绘制：命令表 148 条里没有 Menu 系命令'
                   '（只有 MenuMessSkip(41)/TitleMenu(141)），exe 里也没有菜单文案；'
                   '几何来自 HZC1 头的屏幕坐标字段，不是版面表'),
            system=dict(facts['system'],
                        note='8 项环形菜单，每项 normal/hover/mask 三态；'
                             '打开点在脚本主循环 InputGetState/Down bit1 之后'),
            dialog=dict(facts['dialog'],
                        note='7 个确认提示（是图片，不是文字）+ yes/no 两态 + 光标；'
                             '每项由 enable global 决定是否可用'),
            canvas=dict(virt=[1024, 640], real=[1920, 1080], scale=[1.875, 1.6875]),
            inventory=facts['inventory'],
            unknowns=['菜单项在屏幕上的最终位置由 HZC1 头给出，但 hover 时是否再用'
                      '脚本微调（PrimSetXY）尚未逐项确认',
                      '0x577C8 里 14 个「取反写回」的确切语义未证实',
                      '确认框之外的 save/load 列表几何（slf_*）另属一块，未纳入本轮'],
        )
        with open(a.json, 'w', encoding='utf-8') as fp:
            json.dump(payload, fp, ensure_ascii=False, indent=1)
        print('json -> %s' % a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())