#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""游戏内菜单结构门禁（ref/ingame-menu.json）。

被验证的主张（都来自静态证据，不跑游戏、不抓帧）：

  1. 命令表 148 条里没有 Menu 系命令，只有 MenuMessSkip(41) 与 TitleMenu(141)
     —— 所以游戏内菜单不可能是外壳画的；
  2. exe 里没有菜单文案字符串（菜单文字在图片里：menu_*_mes）；
  3. HCB 里有一个菜单模块（0x57300..0x57800）：7 个菜单项各自把一张
     menu_*_mes 载入图元 94、挂到父组 795（第 3 项「終了」挂 796）、
     由 global 1593..1599 决定该项是否可用；确认框用 95..98（yes/no 各两态）；
     居中锚点是 PrimSetOP(×1.875, ×1.6875) 于 (512,320) 虚拟坐标 = (960,540)。

有游戏数据时重新静态扫描 HCB 做逐字段比对；没有数据时只校验冻结 JSON 的
内部一致性与命令表证据（不伪造通过）。
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF = os.path.join(FVP, 'ref', 'ingame-menu.json')
SCANNER = os.path.join(FVP, 'scripts', 'scan-ingame-menu.py')
CMD_TABLE = os.path.join(FVP, 'ref', 'cmd_table.json')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
HCB = os.path.join(GAME, 'iroseka.hcb')
EXE = os.path.join(GAME, 'iroseka_HD.exe')

MENU_WORDS = ['セーブ', 'ロード', '設定', '終了', '戻す', '上書き',
              '初期化', '削除', 'メニュー']


def load_scanner():
    spec = importlib.util.spec_from_file_location('scan_ingame_menu', SCANNER)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def jp_strings(blob):
    """粗抽 exe 里 NUL 结尾、看起来是日文的字符串（只为做「有没有同名文案」判定）。"""
    out = set()
    for m in re.finditer(rb'[\x20-\x7e\x81-\x9f\xe0-\xfc]{2,}\x00', blob):
        try:
            t = m.group(0)[:-1].decode('cp932')
        except UnicodeDecodeError:
            continue
        if len(t) < 2 or not any('\u3040' <= c <= '\u9fff' for c in t):
            continue
        out.add(t)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    a = ap.parse_args()
    fails = []

    def check(cond, msg):
        print(('  ok    ' if cond else '  FAIL  ') + msg)
        if not cond:
            fails.append(msg)

    if not os.path.isfile(a.json):
        print('verify_ingame_menu: FAIL missing %s' % a.json)
        return 1
    with open(a.json, encoding='utf-8') as f:
        ref = json.load(f)

    print('== 冻结基线自洽性')
    check(ref.get('schema') == 'fvp-ingame-menu/1', 'schema fvp-ingame-menu/1')
    src = ref.get('source', {})
    check(src.get('size') == 4275282, 'source size 4275282 (got %s)' % src.get('size'))
    check(len(src.get('sha256', '')) == 64, 'source sha256 recorded')

    items = ref.get('items', [])
    check(len(items) == 7, '7 menu items (got %d)' % len(items))
    check([it['index'] for it in items] == list(range(1, 8)),
          'item indices 1..7 in order')
    check(all(it['prim'] == 94 for it in items), 'all items load into prim 94')
    check(len({it['image'] for it in items}) == 7, '7 distinct item images')
    check(all(it['image'].startswith('menu_') for it in items),
          'item images are menu_*')
    check([it['enable_global'] for it in items] == list(range(1593, 1600)),
          'enable globals 1593..1599 one per item')
    check([it['parent'] for it in items] == [795, 795, 796, 795, 795, 795, 795],
          'parent groups 795/796 (item 3 -> 796)')

    confirms = ref.get('confirms', [])
    check([c['prim'] for c in confirms] == [95, 96, 97, 98],
          'confirm prims 95..98')
    check([c['image'] for c in confirms] ==
          ['menu_yes_1', 'menu_yes_2', 'menu_no_1', 'menu_no_2'],
          'confirm images yes_1/yes_2/no_1/no_2')

    prims = ref.get('prims', {})
    check(prims.get('background') == 93 and ref.get('background', {}).get('image') == 'menu_bg',
          'background = prim 93 / menu_bg')
    check(prims.get('cursor') == 137 and ref.get('cursor', {}).get('image') == 'menu_chk',
          'cursor = prim 137 / menu_chk')

    cv = ref.get('canvas', {})
    check(cv.get('virt') == [1024, 640] and cv.get('real') == [1920, 1080],
          'virtual 1024x640 -> real 1920x1080')
    check(cv.get('scale') == [1.875, 1.6875], 'scale 1.875 / 1.6875')
    check(cv.get('anchor') == [512, 320], 'anchor at virtual (512,320)')
    check(cv.get('anchor_px') == [960, 540], 'anchor -> real (960,540)')
    check(cv.get('floats') == ['0x3fd80000', '0x3ff00000'],
          'float bit patterns 0x3FD80000 / 0x3FF00000')
    check(abs(512 * 1.875 - 960) < 1e-9 and abs(320 * 1.6875 - 540) < 1e-9,
          'anchor math 512*1.875=960, 320*1.6875=540')

    strings = ref.get('strings', [])
    check(len(strings) == 13, '13 pushstring operands in the module (got %d)'
          % len(strings))
    check(sorted(strings) == sorted(set(strings)), 'no duplicate strings')

    # 7 个开关由谁写：选项面板（47..53）、配置块拷贝（976..982）、重置、菜单自身
    em = ref.get('enable_map', [])
    check(len(em) == 7, '7 enable-map entries (got %d)' % len(em))
    check([m['option_id'] for m in em] == list(range(47, 54)),
          'option ids 47..53')
    check([m['item'] for m in em] == [4, 5, 6, 1, 7, 3, 2],
          'option order is ring order 4,5,6,1,7,3,2 (not 1..7)')
    check([m['config_global'] for m in em] == list(range(976, 983)),
          'config defaults 976..982 in the same order')
    check(sorted(m['global_'] for m in em) == list(range(1593, 1600)),
          'the 7 mappings cover globals 1593..1599 exactly once')
    by_item = {it['index']: it['enable_global'] for it in items}
    check(all(m['global_'] == by_item[m['item']] for m in em),
          'every mapping targets the item\'s own enable flag')
    check(all(m['image'] == items[m['item'] - 1]['image'] for m in em),
          'image names match the item table')

    w = ref.get('enable_writes', [])
    check(len(w) == 35, '35 flag write sites (got %d)' % len(w))
    kinds = {}
    for x in w:
        kinds[(x['fn'], x['value'])] = kinds.get((x['fn'], x['value']), 0) + 1
    check(kinds.get(('0x22f48', 'false')) == 7,
          'init function 0x22F48 clears all 7 flags')
    check(kinds.get(('0x25e93', 'arg')) == 7,
          'option switch 0x25E93 sets all 7 from its argument')
    check(kinds.get(('0x6ce71', 'global:976')) == 1 and
          sum(v for (fn, _v), v in kinds.items() if fn == '0x6ce71') == 7,
          'config copy 0x6CE71 fills all 7 from defaults 976..982')
    check(kinds.get(('0x577c8', 'true')) == 7 and
          kinds.get(('0x577c8', 'false')) == 7,
          'menu 0x577C8 toggles each flag (7 true + 7 false)')

    print('== 命令表证据（菜单不可能是外壳画的）')
    with open(CMD_TABLE, encoding='utf-8') as f:
        table = json.load(f)
    menu_cmds = sorted((e['id'], e['name']) for e in table
                       if 'menu' in e['name'].lower())
    check(menu_cmds == [(41, 'MenuMessSkip'), (141, 'TitleMenu')],
          'only MenuMessSkip(41) / TitleMenu(141) in %d commands' % len(table))

    print('== exe 里没有和菜单项同名的独立文案（文字在图里）')
    if os.path.isfile(EXE):
        blob = open(EXE, 'rb').read()
        labels = jp_strings(blob)
        hits = [w for w in MENU_WORDS if w in labels]
        check(not hits, 'no standalone cp932 menu label in exe (%s)'
              % (hits or 'none'))
        # 「終了」的字节只作为「開くに失敗しました。終了してもよろしいですか？」的一段出现，
        # 不是独立菜单标签（独立标签会以 0x00 结尾）
        raw = '終了'.encode('cp932')
        pos = blob.find(raw)
        tail = blob[pos + len(raw):pos + len(raw) + 4] if pos >= 0 else b''
        check(pos >= 0 and tail == 'して'.encode('cp932'),
              '「終了」 is followed by 「して」 (dialog sentence, not a label)')
        check(b'MenuMessSkip' in blob, 'exe still exports MenuMessSkip name')
        check(b'SaveCreate' in blob, 'exe still exports SaveCreate name')
    else:
        print('  skip  exe not present')

    print('== 重新静态扫描 HCB，逐字段比对')
    if os.path.isfile(HCB):
        mod = load_scanner()
        facts = mod.scan(HCB)
        check(facts['hcb_size'] == src['size'], 'hcb size unchanged')
        check(hashlib.sha256(open(HCB, 'rb').read()).hexdigest() == src['sha256'],
              'hcb sha256 unchanged')
        check(facts['region'] == ref['region'], 'scan region unchanged')
        fres = [dict(index=it['index'], image=it['image'], prim=it['prim'],
                     parent=it['parent'], enable_global=it['enable_global'],
                     load_pc=it['load_pc']) for it in facts['items']]
        check(fres == items, 'items re-derived identically (%d)' % len(fres))
        check([dict(prim=c['prim'], image=c['image']) for c in
               [dict(prim=r['prim'], image=r['image'])
                for r in facts['loads'] if r['prim'] in mod.CONFIRM_PRIMS]]
              == [dict(prim=c['prim'], image=c['image']) for c in confirms],
              'confirm map re-derived identically')
        check(facts['entry']['fn'] == ref['entry']['fn'],
              'entry %s unchanged' % ref['entry']['fn'])
        check(facts['entry']['xrefs'] == ref['entry']['xrefs'],
              '%d call sites to entry unchanged' % len(ref['entry']['xrefs']))
        check(facts['strings'] == strings, 'string list re-derived identically')
        check(facts['floats'] == cv['floats'], 'anchor float constants unchanged')
        check(facts['enable_map'] == em, 'enable map re-derived identically')
        check(facts['writes'] == w, 'flag write sites re-derived identically')
    else:
        print('  skip  hcb not present')

    if fails:
        print('verify_ingame_menu: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_ingame_menu: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())