#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""脚本全局变量（global3 / global236 / global232）的门禁：把"语义未定"变成事实。

已确认（`scripts/extract-script-globals.py` → `ref/script-globals.json`）：

* `global3`：**体验版布局开关**。==1 时体验版标记 `title_trial`(277) 显示、
  菜单 1521/1522、1528/1529 被 `PrimSetXY(prim, true, -48)` 挪到屏幕外隐藏
  （注意：`true` = x 保持原值，`-48` 是 **y 的绝对值**，不是"上移 48"），
  以及 1522/1529 的 `MotionMove` 改走 −81 px 分支。
  两作脚本里 **只有一个写点，写 0**（五彩 0x22F7D、星空 0x36AD4）——
  零售脚本没有任何路径把它置 1 ⇒ 这条体验版分支不可达。
* `global236`（五彩）/ `global232`（星空）：**跳过动画开关**。脚本里既有置 1
  也有置 0 的写点，标题进场/开场大量分支读它（五彩 25 处、星空 24 处）。
  本站不实现它（默认 0 = 正常播放动画）。

本门禁：重新提取 == 基线；断言上面每个数字；断言文档写了结论；断言源码
没有"假装实现体验版布局"的残留（`src/` 里不出现 trial 布局的 48 px 常量）。

缺 HCB 时 SKIP 并返回 0。

用法：python tests/verify_script_globals.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-script-globals.py')
REF = os.path.join(FVP, 'ref', 'script-globals.json')
DOC = os.path.join(FVP, 'docs', 'title-menu.md')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}
EXPECT_SKIP_GLOBAL = {'iroseka': 236, 'hoshimemo': 232}

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    missing = [n for n, p in GAMES.items() if not os.path.isfile(p)]
    if missing:
        print('SKIP: 缺游戏数据 %s' % missing)
        return 0
    for p in (EXTRACT, REF, DOC):
        if not os.path.isfile(p):
            print('FAIL: 缺 %s' % p)
            return 1

    print('== 1. 重新提取并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all',
                           '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-script-globals.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    for name in sorted(ref['games']):
        g = ref['games'][name]
        t = g['trial_layout']
        s = g['skip_animation']
        print('== 2. %s：global%d（体验版布局）==' % (name, t['global']))
        check(t['write_count'] == 1,
              'g%d 只有一个写点（实际 %d）' % (t['global'], t['write_count']))
        check(t['writes'][0]['value'] == 0,
              'g%d 唯一写入的值是 0（实际 %r）'
              % (t['global'], t['writes'][0]['value']))
        check('不可达' in t['conclusion'],
              '结论写明"体验版布局分支不可达"')
        check(t['read_count'] >= 15,
              'g%d 有足够多的读点（%d，>=15）' % (t['global'],
                                                t['read_count']))
        check(t['reads_in_title'] >= 10,
              'g%d 大部分读点在标题域（%d）' % (t['global'],
                                              t['reads_in_title']))

        print('== 3. %s：global%d（跳过动画）==' % (name, s['global']))
        check(s['global'] == EXPECT_SKIP_GLOBAL[name],
              '%s 的跳过动画开关是 g%d（实际 g%d）'
              % (name, EXPECT_SKIP_GLOBAL[name], s['global']))
        vals = {x['value'] for x in s['writes']}
        check(0 in vals and 1 in vals,
              'g%d 既有置 0 也有置 1 的写点（%r）' % (s['global'],
                                                     sorted(vals)))
        check(s['read_count'] >= 20,
              'g%d 读点 >=20（实际 %d）' % (s['global'], s['read_count']))
        check(s['reads_in_title'] >= 15,
              'g%d 大部分读点在标题域（%d）' % (s['global'],
                                              s['reads_in_title']))

    print('== 4. 文档记录结论（§24）==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('体验版布局开关' in doc and '不可达' in doc,
          'title-menu 记录 global3 = 体验版布局开关且零售不可达')
    check('跳过动画开关' in doc, 'title-menu 记录 global236/232 = 跳过动画开关')
    check('y = -48' in doc or '绝对值' in doc,
          'title-menu 写明 -48 是 y 的绝对值（隐藏），不是"上移 48"')
    check('script-globals.json' in doc, '文档指向 ref/script-globals.json')

    print('== 5. 源码没有"假装实现体验版布局"的残留 ==')
    bad = []
    for root, _dirs, files in os.walk(os.path.join(FVP, 'src')):
        for fn in files:
            if not fn.endswith(('.c', '.h')):
                continue
            p = os.path.join(root, fn)
            text = io.open(p, encoding='utf-8', errors='replace').read()
            for line in text.splitlines():
                if 'trial' in line.lower() and '48' in line:
                    bad.append('%s: %s' % (p, line.strip()[:100]))
    check(not bad, 'src/ 里没有 trial-48 残留（%r）' % bad[:3])

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: global3 = 体验版布局开关（零售脚本只写 0 ⇒ 分支不可达）、'
          'global236/232 = 跳过动画开关；两作数字与文档一致')
    return 0


if __name__ == '__main__':
    sys.exit(main())