#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题画面"是谁画的"门禁：脚本侧事实必须与 `ref/title-scene.json` 一致。

为什么要有它：第 121 轮证否了"`TitleMenu`(141) 传布局参数"的猜测之后，标题画面的
归属必须靠**文件里的证据**定下来，而不是靠截图推断。`scripts/extract-title-scene.py`
从 HCB 里抽出这些事实，本门禁把它们冻结：

  1. 重新提取的结果与 `ref/title-scene.json` **逐字节一致**；
  2. 两部作品都用了同一套脚本侧做法：`title_*` 素材字符串 + `GraphLoad` 进槽位
     + `InputGetCursX/Y`、`InputGetDown`、`InputGetState`、`PrimHit` 做交互；
  3. **菜单项就是图元**：五彩斑斓 5 个按钮的图元号是
     `1501 / 1508 / 1515 / 1522 / 1529`（步长 7），星空的记忆是
     `266 / 270 / 272 / 274` 与 `268`（步长 2）—— 命中由引擎 `PrimHit` 按图元
     矩形判定，所以**按钮的身份与矩形都在脚本/图元里，不需要量截图**；
  4. 5 个菜单项素材家族（`title_start/title_continue/title_album/title_option/
     title_end`）在两部作品里都存在，且另有 `title_trial`（体验版按钮）。

缺 HCB 时 SKIP 并返回 0。

用法：python tests/verify_title_scene.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-title-scene.py')
REF = os.path.join(FVP, 'ref', 'title-scene.json')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}
ITEM_PRIMS = {
    'iroseka': [1501, 1508, 1515, 1522, 1529],
    'hoshimemo': [266, 268, 270, 272, 274],
}
ITEM_FAMILIES = ['title_start', 'title_continue', 'title_album', 'title_option',
                 'title_end']

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    missing = [n for n, p in GAMES.items() if not os.path.isfile(p)]
    if missing:
        print('SKIP: 缺游戏数据 %s（可选语料，见 docs/status.md）' % missing)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-title-scene.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    for name in sorted(GAMES):
        r = ref['games'].get(name) or {}
        print('== 2. %s：脚本侧事实 ==' % name)
        fams = r.get('families') or {}
        for f in ITEM_FAMILIES:
            check(f in fams, '%s 有菜单项素材家族 %s（实际变体 %s）'
                  % (name, f, fams.get(f)))
        check('title_trial' in fams, '%s 有 title_trial（体验版按钮）' % name)
        loads = r.get('loads') or []
        check(len(loads) >= 4, '%s 至少有 4 处 title_* 的 GraphLoad（实际 %d）'
              % (name, len(loads)))
        slots = sorted({ld['slot'] for ld in loads})
        check(258 in slots, '%s 的 GraphLoad 用了槽位 258（实际 %s）'
              % (name, slots))
        names = {x['name'] for x in (r.get('title_inputs') or [])}
        for need in ('InputGetCursX', 'InputGetCursY', 'InputGetDown', 'PrimHit'):
            check(need in names, '%s 的标题区用了 %s' % (name, need))
        print('== 3. %s：菜单项就是图元（PrimHit 的图元号） ==' % name)
        check(r.get('menu_prim_ids') == ITEM_PRIMS[name],
              '%s 的 5 个菜单项图元 == %s（实际 %s）'
              % (name, ITEM_PRIMS[name], r.get('menu_prim_ids')))
        hits = [p for p in (r.get('prim_hit') or [])
                if p['prim'] in ITEM_PRIMS[name]]
        check(len(hits) >= 5, '%s 有 >= 5 个菜单项的 PrimHit 调用点（实际 %d）'
              % (name, len(hits)))

    print('== 4. 背景层叠放顺序：来自脚本的 PrimSetZ（layer_z） ==')
    ir = ref['games'].get('iroseka') or {}
    zmap = {}
    for e in (ir.get('layer_z') or []):
        if e['prim'] in (258, 259, 260, 261):
            zmap[e['prim']] = e['z']
    check(zmap == {258: 1000, 259: 900, 260: 850, 261: 800},
          '五彩斑斓背景层 Z：A(258)=1000 / B(259)=900 / C(260)=850 / D(261)=800'
          '（实际 %s）' % zmap)
    check(zmap and max(zmap.values()) - min(zmap.values()) == 200,
          '四层 Z 跨度 200（实际 %s）'
          % (max(zmap.values()) - min(zmap.values()) if zmap else None))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 标题画面由脚本绘制（菜单项 = 图元 + PrimHit），证据已冻结')
    return 0


if __name__ == '__main__':
    sys.exit(main())