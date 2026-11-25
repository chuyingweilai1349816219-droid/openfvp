#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题菜单**状态变体**的门禁：变体是"动画/状态帧"，不是"轻微高亮"。

背景：五彩斑斓每个菜单项除了基准贴图 `title_<item>` 还有 5 个变体
`title_<item>1..5`；脚本把 6 个名字先读进局部变量、再用**变量**做 `GraphLoad`
（`pushstack 4..15` 那种），所以"当前该显示哪个"由脚本的状态机决定。
本门禁用静态像素比对把这件事冻结成事实，而不是靠截图猜：

  1. `scripts/analyze-title-variants.py --check` 与 `ref/title-variants.json`
     **逐字节一致**（解码/统计逻辑漂移立刻红）；
  2. 五彩斑斓：每项**恰好 5 个变体**，且每个变体与基准态差异 **>= 60%** 像素
     → 它们是**整幅不同的状态/动画帧**，不是"hover 只加一点高光"；
  3. 五彩斑斓：变体 1 比基准高 **11 px**（`title_option` 是 12 px），说明它多带
     一条额外内容（很可能是动画/标签区）；
  4. 星空的记忆：每项只有 1 个变体（`title_<item>2`），同样记录。

结论用途：**"hover = 换一个变体贴图"这种简化是错的**，正确做法是解开脚本状态机
（哪个变体在 hover/选中时被画出来）——写进 `docs/title-menu.md` §10.2。
缺游戏目录时 SKIP 并返回 0。

用法：python tests/verify_title_variants.py
"""
import io
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
ANALYZE = os.path.join(FVP, 'scripts', 'analyze-title-variants.py')
REF = os.path.join(FVP, 'ref', 'title-variants.json')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed'),
}
ITEMS = ['title_start', 'title_continue', 'title_album', 'title_option',
         'title_end']

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def main():
    missing = [n for n, p in GAMES.items() if not os.path.isdir(p)]
    if missing:
        print('SKIP: 缺游戏目录 %s（可选语料，见 docs/status.md）' % missing)
        return 0
    if not os.path.isfile(REF):
        print('FAIL: 缺基线 %s' % REF)
        return 1

    print('== 1. 重新分析并与基线逐字节比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', ANALYZE, '--game', 'all', '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=1800)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'analyze-title-variants.py --check 退出码 0（实际 %d）' % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 五彩斑斓：每项 5 个变体，且都是整幅不同的状态帧 ==')
    ir = ref['games'].get('iroseka') or {}
    base_h = {'title_start': 80, 'title_continue': 80, 'title_album': 80,
              'title_option': 90, 'title_end': 80}
    for item in ITEMS:
        e = ir.get(item)
        if not check(e is not None, '五彩斑斓有 %s 的变体统计' % item):
            continue
        names = sorted(e['variants'])
        want = ['%s%d' % (item, k) for k in range(1, 6)]
        check(names == want, '%s 的变体名恰好 = %s（实际 %s）' % (item, want, names))
        for vn in names:
            st = e['variants'][vn]
            check(st['diff_pct'] >= 60.0,
                  '%s 与基准差异 >= 60%%（实际 %.2f%%）'
                  % (vn, st['diff_pct']))
        # 变体 1 比基准更高（多一条内容），这是"整幅状态帧"的直接证据。
        # 五彩斑斓的基准高度分别是 80/80/80/90/80，变体 1 是 91/91/91/92/92
        # ——option 的基准本身就是 90（素材头如此），所以差值只 +2。
        v1 = e['variants'].get('%s1' % item) or {}
        check(v1.get('h', 0) in (91, 92),
              '%s1 的高度是 91/92（实际 %s，基准 %s）'
              % (item, v1.get('h'), base_h[item]))

    print('== 2.5 映射：变体 1 = 高亮态；变体 2..5 = alpha 单调递减的淡出帧 ==')
    for item in ITEMS:
        e = ir.get(item) or {}
        variants = e.get('variants') or {}
        base_mean = (e.get('base') or {}).get('mean_bgra') or [0, 0, 0, 0]
        v1 = (variants.get('%s1' % item) or {}).get('mean_bgra') or [0, 0, 0, 0]
        # 高亮态：色相明显更"蓝"（B - R 拉大），alpha 不下降
        check(v1[0] - v1[2] >= (base_mean[0] - base_mean[2]) + 40,
              '%s1 是偏蓝的高亮态（B-R %d vs 基准 %d）'
              % (item, v1[0] - v1[2], base_mean[0] - base_mean[2]))
        check(v1[3] >= base_mean[3] - 5,
              '%s1 的 alpha 不下降（%d vs %d）' % (item, v1[3], base_mean[3]))
        # 淡出帧：2..5 的 alpha 严格递减
        alphas = [(variants.get('%s%d' % (item, k)) or {}).get('mean_bgra', [0, 0, 0, 0])[3]
                  for k in range(2, 6)]
        check(all(alphas[i] > alphas[i + 1] for i in range(len(alphas) - 1)),
              '%s 的变体 2..5 alpha 严格递减（实际 %s）' % (item, alphas))
        check(alphas[-1] < base_mean[3] // 2,
              '%s5 的 alpha 不到基准的一半（%d vs %d）'
              % (item, alphas[-1], base_mean[3]))

    print('== 2.6 实现里的 hover 贴图名 == asset + "1"（src/titlemenu.c） ==')
    src = os.path.join(FVP, 'src', 'titlemenu.c')
    with io.open(src, encoding='utf-8') as fp:
        text = fp.read()
    lo = text.find('fvp_title_profile_iroseka = {')
    hi = text.find('fvp_title_profile_hoshimemo = {')
    block = text[lo:hi] if lo >= 0 and hi > lo else text
    pat = re.compile(r'\{\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"\s*,')
    pairs = {m.group(2): m.group(3) for m in pat.finditer(block)}
    for item in ITEMS:
        check(pairs.get(item) == '%s1' % item,
              '五彩斑斓 %s 的 hover 贴图 = %s1（实际 %r）'
              % (item, item, pairs.get(item)))

    print('== 3. 星空：每项 1 个变体（title_<item>2） ==')
    ho = ref['games'].get('hoshimemo') or {}
    for item in ITEMS:
        e = ho.get(item)
        if not check(e is not None, '星空有 %s 的变体统计' % item):
            continue
        check(sorted(e['variants']) == ['%s2' % item],
              '%s 只有 1 个变体 %s2（实际 %s）'
              % (item, item, sorted(e['variants'])))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 状态变体是整幅不同的帧（五彩斑斓 5 个/项、星空 1 个/项），'
          'hover 不能用"换一个变体"简单实现')
    return 0


if __name__ == '__main__':
    sys.exit(main())