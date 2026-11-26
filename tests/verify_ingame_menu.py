#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""游戏内菜单结构门禁（ref/ingame-menu.json，schema v2）。

被验证的主张（全部来自静态证据，不跑游戏、不抓帧）：

  1. 命令表 148 条里没有 Menu 系命令，只有 MenuMessSkip(41) 与 TitleMenu(141)
     —— 所以系统菜单不可能是外壳画的；exe 里也没有菜单文案（文字在图片里）；
  2. **系统菜单**：8 项 × 3 态（normal/hover/mask），图元 141..164 连续，
     素材 `sys_{save,load,hidewin,undo,config,title,exit,return}_{1,2,M}`；
     打开点在脚本主循环 `InputGetState/InputGetDown` bit1 之后（5 个调用点）；
  3. **确认框**：7 张提示图（是图片，不是文字）+ yes/no 两态 + 光标；
     每项由 `enable_global` 决定是否可用，选项号 47..53 / 配置默认 976..982
     两条独立路径给出同一张表；
  4. **几何**来自 HZC1 头 `0x18/0x1A`（屏幕坐标）与 `0x14/0x16`（宽高）：
     环形菜单的 8 个矩形构成一条自右上到左下的弧，确认框的提示图/背景/按钮
     全部以 (960,540) 为中心。

有游戏数据与 HZC1 解包目录时重新扫描逐字段比对；缺失时只校验冻结 JSON 的
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
EXTRACT = os.path.join(FVP, 'extract', 'graph')
GAME = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD')
HCB = os.path.join(GAME, 'iroseka.hcb')
EXE = os.path.join(GAME, 'iroseka_HD.exe')

MENU_WORDS = ['セーブ', 'ロード', '設定', '終了', '戻す', '上書き',
              '初期化', '削除', 'メニュー']
SYS_ACTIONS = ['save', 'load', 'hidewin', 'undo', 'config', 'title', 'exit', 'return']
DLG_ACTIONS = ['undo', 'title', 'exit', 'qload', 'overwrite', 'delete', 'init']


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


def strip_rects(node):
    """重扫比对时忽略几何（几何来自 extract/，没有解包目录时扫不出来）。"""
    if isinstance(node, dict):
        return {k: strip_rects(v) for k, v in node.items()
                if k not in ('rect', 'mask_rect', 'hover_rect')}
    if isinstance(node, list):
        return [strip_rects(v) for v in node]
    return node


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=REF)
    ap.add_argument('--extract', default=EXTRACT)
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

    print('== 冻结基线自洽性 / 系统菜单')
    check(ref.get('schema') == 'fvp-ingame-menu/2', 'schema fvp-ingame-menu/2')
    src = ref.get('source', {})
    check(src.get('size') == 4275282, 'source size 4275282 (got %s)' % src.get('size'))
    check(len(src.get('sha256', '')) == 64, 'source sha256 recorded')

    sysm = ref.get('system', {})
    items = sysm.get('items', [])
    check(len(items) == 8, '8 ring items (got %d)' % len(items))
    check([it['order'] for it in items] == list(range(1, 9)), 'orders 1..8')
    check([it['action'] for it in items] == SYS_ACTIONS,
          'actions save/load/hidewin/undo/config/title/exit/return')
    normals = [it['prims']['normal'] for it in items]
    check(normals == [141 + 3 * k for k in range(8)],
          'normal prims 141,144,...,162 (got %s)' % normals)
    check(all(it['prims']['hover'] == it['prims']['normal'] + 1 and
              it['prims']['mask'] == it['prims']['normal'] + 2 for it in items),
          'hover = normal+1 and mask = normal+2 for every item')
    check(all(it['assets']['normal'].startswith('sys_') and
              it['assets']['normal'].endswith('_1') and
              it['assets']['hover'].endswith('_2') and
              it['assets']['mask'].endswith('_M') for it in items),
          'asset naming sys_<action>_{1,2,M}')

    rects = [it.get('rect') for it in items]
    check(all(r and len(r) == 4 and r[2] > 0 and r[3] > 0 for r in rects),
          'every item has a header rect [x,y,w,h]')
    if all(rects):
        check(all(0 <= r[0] < 1920 and 0 <= r[1] < 1080 for r in rects),
              'ring rects inside 1920x1080')
        ys = [r[1] for r in rects]
        check(ys == sorted(ys) and len(set(ys)) == 8,
              'ring descends monotonically (y 229..995)')
        xs = [r[0] for r in rects]
        check(xs[0] >= 1500 and xs[-1] <= 900,
              'arc runs from the right edge (x %d) to the lower left (x %d)'
              % (xs[0], xs[-1]))
        diffs = [it['mask_rect'][1] - it['rect'][1] for it in items]
        check(all(20 <= d <= 24 for d in diffs),
              'mask strip sits 20..24 px below its item (got %s)' % diffs)
        check(all(it['mask_rect'][2] == 874 for it in items),
              'all mask strips are 874 px wide')

    win = sysm.get('window', [])
    check([w['asset'] for w in win] ==
          ['sys_window_title', 'sys_window_right', 'sys_window_left'],
          'window parts title/right/left')
    check([w['prim'] for w in win] == [172, 420, 421],
          'window prims 172/420/421')
    check(len(sysm.get('openers', [])) == 5,
          '5 call sites open the system menu')
    check(any(o['pc'] == '0x5b596' for o in sysm.get('openers', [])),
          'story loop opens it at 0x5B596 (after InputGetState/Down bit1)')

    print('== 冻结基线自洽性 / 确认框')
    dlg = ref.get('dialog', {})
    prompts = dlg.get('prompts', [])
    check(len(prompts) == 7, '7 confirmation prompts (got %d)' % len(prompts))
    check([p['action'] for p in prompts] == DLG_ACTIONS, 'prompt action order')
    check(all(p['prim'] == 94 for p in prompts), 'all prompts share prim 94')
    check([p['parent'] for p in prompts] == [795, 795, 796, 795, 795, 795, 795],
          'only the exit prompt uses parent group 796')
    check([p['enable_global'] for p in prompts] == list(range(1593, 1600)),
          'enable globals 1593..1599')
    check(len({p['image'] for p in prompts}) == 7, '7 distinct prompt images')
    check(all(p['image'].endswith('_mes') and p.get('rect') for p in prompts),
          'prompt images are menu_*_mes with header rects')
    if all(p.get('rect') for p in prompts):
        centers = [p['rect'][0] + p['rect'][2] / 2 for p in prompts]
        check(all(abs(c - 960) <= 12 for c in centers),
              'prompts horizontally centred on x=960 (centres %s)'
              % [round(c, 1) for c in centers])
        check(all(abs(p['rect'][1] - 459) <= 2 for p in prompts),
              'prompts sit on the same baseline (y 458..460)')

    confirms = dlg.get('confirms', [])
    check([c['prim'] for c in confirms] == [95, 96, 97, 98], 'confirm prims 95..98')
    check([c['image'] for c in confirms] ==
          ['menu_yes_1', 'menu_yes_2', 'menu_no_1', 'menu_no_2'],
          'confirm images yes_1/yes_2/no_1/no_2')
    yes = confirms[0].get('rect')
    no = confirms[2].get('rect')
    if yes and no:
        check(yes[0] + yes[2] / 2 < 960 < no[0] + no[2] / 2,
              'yes button is left of centre, no button right of it')

    bg = dlg.get('background', {})
    check(bg.get('prim') == 93 and bg.get('image') == 'menu_bg',
          'dialog background = prim 93 / menu_bg')
    if bg.get('rect'):
        check(abs(bg['rect'][0] + bg['rect'][2] / 2 - 960) <= 2 and
              abs(bg['rect'][1] + bg['rect'][3] / 2 - 540) <= 2,
              'dialog background centred on (960,540)')
    cur = dlg.get('cursor', {})
    check(cur.get('prim') == 137 and cur.get('image') == 'menu_chk',
          'dialog cursor = prim 137 / menu_chk')

    print('== 开关：选项号 / 配置默认值 / 运行时开关')
    em = dlg.get('enable_map', [])
    check(len(em) == 7, '7 enable-map entries (got %d)' % len(em))
    check([m['option_id'] for m in em] == list(range(47, 54)), 'option ids 47..53')
    check([m['action'] for m in em] ==
          ['qload', 'overwrite', 'delete', 'undo', 'init', 'exit', 'title'],
          'option order is NOT ring order (qload..title)')
    check([m['config_global'] for m in em] == list(range(976, 983)),
          'config defaults 976..982 in the same order')
    check(sorted(m['global_'] for m in em) == list(range(1593, 1600)),
          'runtime globals 1593..1599 covered exactly once')
    w = dlg.get('writes', [])
    check(len(w) == 35, '35 flag write sites (got %d)' % len(w))
    kinds = {}
    for x in w:
        kinds[(x['fn'], x['value'])] = kinds.get((x['fn'], x['value']), 0) + 1
    check(kinds.get(('0x22f48', 'false')) == 7, 'init 0x22F48 clears all 7')
    check(kinds.get(('0x25e93', 'arg')) == 7, 'option switch 0x25E93 sets all 7')
    check(sum(v for (fn, _v), v in kinds.items() if fn == '0x6ce71') == 7,
          'config copy 0x6CE71 fills all 7 from defaults')
    check(kinds.get(('0x577c8', 'true')) == 7 and kinds.get(('0x577c8', 'false')) == 7,
          'dialog code toggles each flag (7 true + 7 false)')

    for p in prompts:
        check(len(p.get('call_sites', [])) >= 1,
              '%s prompt has at least one trigger site' % p['action'])
    check(len(dlg.get('strings', [])) == 13, '13 pushstring operands in the dialog')

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
        hits = [x for x in MENU_WORDS if x in labels]
        check(not hits, 'no standalone cp932 menu label in exe (%s)' % (hits or 'none'))
        raw = '終了'.encode('cp932')
        pos = blob.find(raw)
        tail = blob[pos + len(raw):pos + len(raw) + 4] if pos >= 0 else b''
        check(pos >= 0 and tail == 'して'.encode('cp932'),
              '「終了」 is followed by 「して」 (dialog sentence, not a label)')
        check(b'MenuMessSkip' in blob and b'SaveCreate' in blob,
              'exe still exports MenuMessSkip / SaveCreate names')
    else:
        print('  skip  exe not present')

    print('== 重新静态扫描 HCB，逐字段比对')
    if os.path.isfile(HCB):
        mod = load_scanner()
        ext = a.extract if os.path.isdir(a.extract) else None
        facts = mod.scan(HCB, ext)
        check(facts['hcb_size'] == src['size'], 'hcb size unchanged')
        check(hashlib.sha256(open(HCB, 'rb').read()).hexdigest() == src['sha256'],
              'hcb sha256 unchanged')
        got_sys = strip_rects(facts['system'])
        ref_sys = strip_rects(sysm)
        check(got_sys['items'] == ref_sys['items'], 'ring items re-derived identically')
        check(got_sys['window'] == ref_sys['window'], 'window parts identical')
        check(got_sys['openers'] == ref_sys['openers'], 'open sites identical')
        check(got_sys['entry'] == ref_sys['entry'], 'system entry unchanged')
        got_dlg = strip_rects(facts['dialog'])
        ref_dlg = strip_rects(dlg)
        check(got_dlg['prompts'] == ref_dlg['prompts'], 'prompts identical')
        check(got_dlg['confirms'] == ref_dlg['confirms'], 'yes/no identical')
        check(got_dlg['enable_map'] == ref_dlg['enable_map'], 'enable map identical')
        check(got_dlg['writes'] == ref_dlg['writes'], 'flag writes identical')
        check(got_dlg['sites'] == ref_dlg['sites'], 'trigger sites identical')
        if ext:
            rects_now = [it.get('rect') for it in facts['system']['items']]
            check(rects_now == rects, 'ring rects re-read from HZC1 headers')
        else:
            print('  skip  HZC1 extract dir not present (rects not re-derived)')
    else:
        print('  skip  hcb not present')

    if fails:
        print('verify_ingame_menu: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_ingame_menu: PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())