#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-ingame-menu.py -- 从 HCB 静态抽出「游戏内菜单」的图元/图片表。

为什么要这个：FVP 的 148 条命令表里**没有任何 Menu 系命令**（只有
MenuMessSkip(41) 与 TitleMenu(141)），exe 里也找不到菜单文案字符串。也就是说
《五彩斑斓的世界 HD》的游戏内菜单不是外壳画的，而是脚本自己画：

    0x57383  载入 menu_bg / menu_chk / menu_*_mes 到固定图元号
    0x577C8  按「当前项下标」刷新（选图、挂组、按 enable global 决定光标）
    0x572C9 / 0x57325  淡入淡出（MotionAlpha(42)）
    0x57A07+ 输入与命中（InputGetCursX/Y(29/30) + PrimHit(76)）

本脚本只输出「名字 / 图元号 / 偏移 / 开关 global 号」这类结构事实，**不含任何
图像数据**；产物是 ref/ingame-menu.json，门禁 tests/verify_ingame_menu.py。

用法：
    python scripts/scan-ingame-menu.py                 # 人读摘要
    python scripts/scan-ingame-menu.py --json out/x.json
"""
import argparse
import hashlib
import json
import os
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

# 扫描窗口：菜单模块所在的字节区间。窗口外命中说明脚本换了，门禁会报。
REGION = (0x57300, 0x57800)
WRAP_LOAD = 0x2354B      # (prim, name, ?, ?) -> GraphLoad("graph/"+name)
WRAP_GROUP = 0x235E8     # (prim, parent, ?) -> PrimGroupIn(73)
ANCHOR_HELPER = 0x35AB7  # (prim, 512, 320, on) -> PrimSetOP(×1.875, ×1.6875)
SHOW_HELPER = 0x23804    # (prim, on) -> PrimSetDraw(79)
SPRT_HELPER = 0x35BCC    # (prim, slot, x, y) -> PrimSetSprt(85)，x/y 传 true 则不覆盖
ENTRY = 0x577C8          # (item index) -> 刷新该菜单项
ITEM_PRIM = 94
BG_PRIM = 93
CURSOR_PRIM = 137
CONFIRM_PRIMS = (95, 96, 97, 98)


def _window(code, lo, hi):
    pcs = [p for p in sorted(code.instr) if lo <= p < hi]
    return [(p,) + code.instr[p] for p in pcs]


def scan(path):
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    seq = _window(code, *REGION)

    def at(i):
        return seq[i] if 0 <= i < len(seq) else (None, None, None, [])

    items, loads, cur = [], [], None
    for i, (pc, op, name, args) in enumerate(seq):
        p1, _, n1, a1 = at(i + 1)
        p2, _, n2, a2 = at(i + 2)
        if name == 'pushstack' and args == [254] and n1 == 'pushint8' and n2 == 'eq':
            cur = a1[0]
        if name != 'pushint8':
            continue
        prim = args[0]
        if n1 != 'pushstring' or not a1 or not a1[0].startswith('menu'):
            continue
        # 后面必须紧跟 load 包装调用（pushint8/pushstring/pushtrue/pushtrue/call）
        call_at = None
        for j in range(i + 2, min(i + 6, len(seq))):
            q = seq[j]
            if q[2] == 'call':
                if q[3] and q[3][0] == WRAP_LOAD:
                    call_at = j
                break
        if call_at is None:
            continue
        parent = None
        for j in range(i + 4, min(i + 14, len(seq))):
            q = seq[j]
            if q[2] == 'pushint16' and q[3] and q[3][0] in (795, 796):
                parent = q[3][0]
                break
            if q[2] == 'pushint8' and j > i + 6:
                break
        enable = None
        for j in range(i + 4, min(i + 16, len(seq))):
            q = seq[j]
            if q[2] == 'pushglobal':
                enable = q[3][0]
                break
        rec = dict(prim=prim, image=a1[0], load_pc='0x%05x' % pc,
                   parent=parent, enable_global=enable)
        loads.append(dict(rec))
        if prim == ITEM_PRIM:
            rec['index'] = cur
            items.append(rec)

    items.sort(key=lambda r: (r['index'] is None, r['index']))

    # 谁写这 7 个开关：
    #   a) 选项面板 switch（`pushstack 253 / pushint8 <选项号> / eq` + `pushstack 254 / popglobal G`）
    #   b) 配置块拷贝（`pushglobal A / popglobal G`，A 是持久化默认值）
    #   c) 重置（`pushfalse / popglobal G`，在 0x22F48 的初始化函数里）
    by_global = {it['enable_global']: it for it in items if it.get('enable_global')}
    allpcs = sorted(code.instr)
    pos = {p: i for i, p in enumerate(allpcs)}
    fn_starts = [p for p in allpcs if code.instr[p][1] == 'initstack']

    def step(pc, n):
        i = pos.get(pc)
        if i is None or not (0 <= i + n < len(allpcs)):
            return (None, None, None, [])
        q = allpcs[i + n]
        return (q,) + code.instr[q]

    import bisect

    def fn_of(pc):
        k = bisect.bisect_right(fn_starts, pc) - 1
        return fn_starts[k] if k >= 0 else None

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
            value = 'arg'          # 值由调用方压栈（选项面板的 switch 走这条）
        else:
            value = pn or '?'
        fnpc = fn_of(pc)
        writes.append(dict(pc='0x%05x' % pc, global_=g,
                           fn=('0x%05x' % fnpc) if fnpc is not None else '-',
                           value=value))
        if pn == 'pushstack' and opt is not None:
            it = by_global[g]
            enable_map.append(dict(option_id=opt, global_=g, item=it['index'],
                                   image=it['image'], write_pc='0x%05x' % pc))
    enable_map.sort(key=lambda r: (r['option_id'] is None, r['option_id']))

    # 配置块拷贝：`pushglobal <持久化默认> / popglobal <运行时>`，按写入顺序给出
    # option_id ↔ 默认 global 的对应（与选项面板的编号应完全一致，可互相印证）
    copy_by_global = {w['global_']: w['value'] for w in writes
                      if w['value'].startswith('global:')}
    for m in enable_map:
        src = copy_by_global.get(m['global_'])
        m['config_global'] = int(src.split(':')[1]) if src else None

    strings = [a[0] for _, _, n, a in seq if n == 'pushstring' and a]
    loader_calls = sum(1 for _, _, n, a in seq if n == 'call' and a and a[0] == WRAP_LOAD)
    group_calls = sum(1 for _, _, n, a in seq if n == 'call' and a and a[0] == WRAP_GROUP)
    anchor_calls = sum(1 for _, _, n, a in seq if n == 'call' and a and a[0] == ANCHOR_HELPER)

    # 锚点助手体里的浮点常量：1.875 = 1920/1024、1.6875 = 1080/640
    helper = _window(code, ANCHOR_HELPER, ANCHOR_HELPER + 0x60)
    floats = sorted({a[0] for _, _, n, a in helper if n == 'pushfloat'})

    xrefs = ['0x%05x' % pc for pc in sorted(code.instr)
             if code.instr[pc][1] == 'call' and code.instr[pc][2]
             and code.instr[pc][2][0] == ENTRY]

    return dict(
        hcb_size=len(data),
        hcb_sha256=hashlib.sha256(data).hexdigest(),
        region=['0x%05x' % REGION[0], '0x%05x' % REGION[1]],
        instructions=len(seq),
        loader_calls=loader_calls,
        group_calls=group_calls,
        anchor_calls=anchor_calls,
        floats=['0x%08x' % f for f in floats],
        strings=strings,
        items=items,
        loads=loads,
        enable_map=enable_map,
        writes=writes,
        entry=dict(fn='0x%05x' % ENTRY, xrefs=xrefs),
    )


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--game', default='iroseka', choices=sorted(GAMES))
    ap.add_argument('--json', default='')
    a = ap.parse_args(argv)
    path = GAMES[a.game]
    if not os.path.isfile(path):
        print('SKIP: %s not found' % path)
        return 0
    facts = scan(path)
    print('%s: hcb %d B sha256 %s' % (a.game, facts['hcb_size'],
                                      facts['hcb_sha256'][:16]))
    print('region %s..%s, %d instructions, %d loader call(s), %d group call(s)'
          % (facts['region'][0], facts['region'][1], facts['instructions'],
             facts['loader_calls'], facts['group_calls']))
    print('strings in region: %s' % ', '.join(facts['strings']))
    for it in facts['items']:
        print('  item %s  %-16s prim %d parent %s enable global %s  (%s)'
              % (it['index'], it['image'], it['prim'], it['parent'],
                 it['enable_global'], it['load_pc']))
    print('entry %s called from %d site(s)' % (facts['entry']['fn'],
                                               len(facts['entry']['xrefs'])))
    for m in facts['enable_map']:
        print('  option %s -> global[%d] = item %s (%s)  config global %s  @%s'
              % (m['option_id'], m['global_'], m['item'], m['image'],
                 m['config_global'], m['write_pc']))
    print('flag writes: %d site(s) in %d function(s)'
          % (len(facts['writes']), len({w['fn'] for w in facts['writes']})))
    if a.json:
        payload = dict(
            schema='fvp-ingame-menu/1', game=a.game,
            source=dict(file=os.path.basename(path), size=facts['hcb_size'],
                        sha256=facts['hcb_sha256'],
                        method='HCB 静态解码（ref/fvm_ref.py），只读结构，不含图像'),
            claim=('游戏内菜单由脚本绘制：命令表 148 条里没有 Menu*（只有 '
                   'MenuMessSkip(41) 与 TitleMenu(141)），exe 也没有菜单文案字符串'),
            region=facts['region'],
            module=dict(load='0x%05x' % WRAP_LOAD, group='0x%05x' % WRAP_GROUP,
                        anchor='0x%05x' % ANCHOR_HELPER, show='0x%05x' % SHOW_HELPER,
                        sprt='0x%05x' % SPRT_HELPER,
                        entry=facts['entry']['fn']),
            canvas=dict(virt=[1024, 640], real=[1920, 1080], scale=[1.875, 1.6875],
                        anchor=[512, 320], anchor_px=[960, 540],
                        floats=facts['floats']),
            prims=dict(background=BG_PRIM, item=ITEM_PRIM, cursor=CURSOR_PRIM,
                       confirm=list(CONFIRM_PRIMS),
                       parents=[795, 796]),
            items=facts['items'],
            confirms=[dict(prim=r['prim'], image=r['image'], load_pc=r['load_pc'])
                      for r in facts['loads'] if r['prim'] in CONFIRM_PRIMS],
            background=dict(prim=BG_PRIM, image='menu_bg'),
            cursor=dict(prim=CURSOR_PRIM, image='menu_chk'),
            enable_map=facts['enable_map'],
            enable_writes=facts['writes'],
            strings=facts['strings'],
            entry=facts['entry'],
            unknowns=['菜单打开/关闭由哪条脚本路径触发（0x577C8 的 12 个调用点尚未逐个确认）',
                      '图元号 795/796 两个挂接父组的用途（推测为普通项/危险项两组）',
                      '菜单项的屏幕坐标：本区间内没有显式 PrimSetXY，位置由上层 Motion/布局决定',
                      '0x577C8 里 14 个「取反写回」的确切语义（看着像按项切换可用性 + 同步光标，未证实）'],
        )
        with open(a.json, 'w', encoding='utf-8') as fp:
            json.dump(payload, fp, ensure_ascii=False, indent=1)
        print('json -> %s' % a.json)
    return 0


def _unused():
    return None


if __name__ == '__main__':
    sys.exit(main())