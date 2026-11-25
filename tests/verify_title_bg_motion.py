#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""标题背景的缩放/旋转参数门禁（两个作品）：回答"旋转能不能从解包解出来"。

结论（由本门禁冻结）：
  * **图像包（graph.bin / HZC1）里没有旋转信息**——44 字节头只有 w/h/x/y/fmt；
  * **旋转/缩放在 HCB 脚本的 `MotionMoveR`(51) / `MotionMoveS2`(54) 调用里**，
    实参是字面量（角度单位 0.1°，时长/缓动见 handler 0x43AE50 / 0x43AF70）；
  * 五彩斑斓：背景 4 层 258..261，缩放 1300/1600/2500/4000 → 900/800/800/800、
    旋转 150/250/550/1250（=15°/25°/55°/125°）→ 0，都是 25000 ms / mode 3；
    OP 全部 (1200,675)（设计画布 2400×1350 中心），`PrimSetRS(258, keep, 1000)` 被
    脚本写了 4 次（原文就是一直写 258，不是我们解析错）。
  * 星空：背景只有槽 258（依次装 title_bg3/title_bg2/title_bg1），
    只有一次 `MotionMoveS2 2000→1000`、**时长 40000 ms**（0x6FD67 的字面量），
    **没有 MotionMoveR**（背景不旋转），也没有 PrimSetOP。

缺 HCB 时 SKIP 并返回 0。

用法：python tests/verify_title_bg_motion.py
"""
import io
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-title-bg-motion.py')
REF = os.path.join(FVP, 'ref', 'title-bg-motion.json')
GAMES = {
    'iroseka': os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                            'iroseka.hcb'),
    'hoshimemo': os.path.join(r'C:\Users\Mechrevo\Downloads', 'hoshimemo_installed',
                              'Hoshimemo_HD.hcb'),
}

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
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--game', 'all',
                           '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=900)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    check(proc.returncode == 0,
          'extract-title-bg-motion.py --check 退出码 0（实际 %d）'
          % proc.returncode)

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 五彩斑斓：4 层背景的缩放 + 旋转 ==')
    g = ref['games']['iroseka']
    check(g['bg_slots'] == [258, 259, 260, 261], '背景槽 258..261')
    s2 = [m for m in g['motions'] if m['name'] == 'MotionMoveS2']
    rr = [m for m in g['motions'] if m['name'] == 'MotionMoveR']
    check(len(s2) == 4 and len(rr) == 4, '4 条缩放 + 4 条旋转（实际 %d/%d）'
          % (len(s2), len(rr)))
    want_scale = {258: (1300, 900), 259: (1600, 800), 260: (2500, 800),
                  261: (4000, 800)}
    want_rot = {258: 150, 259: 250, 260: 550, 261: 1250}
    for m in s2:
        w = want_scale[m['prim']]
        check(m['args'][1] == w[0] and m['args'][2] == w[1],
              'prim %d 缩放 %s→%s（实际 %r）'
              % (m['prim'], w[0], w[1], m['args'][1:3]))
        check(m['args'][6] == 3, 'prim %d 缩放 mode 3' % m['prim'])
    for m in rr:
        check(m['args'][1] == want_rot[m['prim']] and m['args'][2] == 0,
              'prim %d 旋转 %s→0（0.1° 单位 = %s°→0°，实际 %r）'
              % (m['prim'], want_rot[m['prim']], want_rot[m['prim']] / 10.0,
                 m['args'][1:3]))
        check(m['args'][4] == 3, 'prim %d 旋转 mode 3' % m['prim'])
    # 时长来自栈局部 7（正常分支 25000、跳过分支 1）。块内包含赋值时能直接解成
    # 25000；块从 syscall 后开始时只能给出候选 {1, 25000} —— 两种都认，
    # 但必须至少出现一次"候选"形式，证明我们没把分支歧义藏起来。
    durs = [m['args'][5] for m in s2] + [m['args'][3] for m in rr]
    has_dict = False
    for d in durs:
        if isinstance(d, int):
            check(d == 25000, '时长字面量 25000（实际 %r）' % d)
            continue
        if isinstance(d, dict):
            has_dict = True
            cand = {c.get('value') for c in (d.get('candidates') or [])
                    if isinstance(c.get('value'), int)}
            check(d.get('local') == 7 and {1, 25000} <= cand,
                  '时长来自栈局部 7，候选值含 1 / 25000（%s）'
                  % sorted(cand))
        else:
            check(False, '无法识别的时长表示 %r' % (d,))
    check(has_dict, '至少有一部分运动把时长记成"栈局部 + 分支候选"（诚实记录）')

    print('== 3. 五彩斑斓：OP / Z / RS 与素材头 ==')
    ops = {(op['cmd'], op['prim']): op['args'] for op in g['prim_ops']}
    for prim in (258, 259, 260, 261):
        check(ops.get((81, prim)) == [prim, 1200, 675],
              'prim %d 的 PrimSetOP = (1200,675)（设计画布中心）' % prim)
    zs = {op['prim']: op['args'][1] for op in g['prim_ops'] if op['cmd'] == 91}
    check(zs == {258: 1000, 259: 900, 260: 850, 261: 800},
          '背景 Z = 1000/900/850/800（实际 %r）' % zs)
    rs = [op for op in g['prim_ops'] if op['cmd'] == 82]
    check(len(rs) == 4 and all(op['prim'] == 258 for op in rs),
          'PrimSetRS 被写了 4 次且原文都写 prim 258（脚本原样）')
    a = g['assets']
    check(a['title_bg1_A']['w'] == 2400 and a['title_bg1_A']['h'] == 1350,
          'title_bg1_A = 2400x1350（超尺寸，需要裁剪）')
    check(a['title_bg1_B']['w'] == 781 and a['title_bg1_B']['x'] == 518,
          'title_bg1_B = 781x1350 @x=518')
    check(a['title_bg1_D']['h'] == 1230 and a['title_bg1_D']['y'] == 111,
          'title_bg1_D = 1955x1230 @y=111')

    print('== 4. 图像包里没有旋转信息 ==')
    for nm, hdr in a.items():
        check(set(hdr) <= {'w', 'h', 'x', 'y', 'pkg'},
              '%s 的 HZC1 头只有 w/h/x/y/fmt（实际键 %s）'
              % (nm, sorted(hdr)))

    print('== 5. 星空：背景只有一层、只缩放不旋转 ==')
    h = ref['games']['hoshimemo']
    check(h['bg_slots'] == [258], '背景槽只有 258')
    check(len(h['loads']) == 3, '槽 258 依次装了 3 张背景（实际 %d）'
          % len(h['loads']))
    check([x['asset'] for x in h['loads']] ==
          ['title_bg3', 'title_bg2', 'title_bg1'],
          '装载顺序 title_bg3 → title_bg2 → title_bg1')
    check(len(h['motions']) == 1 and h['motions'][0]['name'] == 'MotionMoveS2',
          '只有 1 条 MotionMoveS2（背景不旋转）')
    m = h['motions'][0]
    check(m['prim'] == 258 and m['args'][1] == 2000 and m['args'][2] == 1000,
          '星空背景缩放 2000→1000（实际 %r）' % (m['args'][1:3],))
    check(m['args'][5] == 40000, '星空背景缩放时长 = 40000 ms（实际 %r）'
          % (m['args'][5],))
    check(not [x for x in h['prim_ops']],
          '星空背景没有 PrimSetOP/RS/Z（默认 OP=(0,0)）')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 背景旋转来自 HCB 脚本（五彩 15/25/55/125°→0、25000 ms；星空不旋转），'
          '图像包里没有旋转信息')
    return 0


if __name__ == '__main__':
    sys.exit(main())