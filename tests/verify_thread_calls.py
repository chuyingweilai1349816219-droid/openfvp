#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_thread_calls.py -- 冻结"线程/标题命令调用点"的脚本事实。

背景（CHANGELOG 八十八续）：标题卡在 `0x592DF` 等 `MotionAlphaTest(265)`，
候选之一是"菜单跑在 slot 9 的协程里"。这条门禁用
`scripts/scan-thread-calls.py` 静态扫描 iroseka 的 HCB，断言：

  1. `ThreadStart(136)` 站点 35 个，用到的槽号**不包含 9**；
  2. `ThreadExit(9)` 出现在 `0x59E50/0x59EC8/0x59F09/0x5A518` 四处；
  3. `TitleMenu(141)` 在函数入口解码集合里出现 5 处（0x5A858/0x5F5BB/0x6133D/0x61367/0x64073；早期字节模式扫到的 0x5A794 不在该集合）。

缺游戏数据时 SKIP 并返回 0（与其它 verify_* 一致）。
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCAN = os.path.join(ROOT, 'scripts', 'scan-thread-calls.py')
HCB = os.path.join(r'C:\Users\Mechrevo\Downloads',
                   '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD', 'iroseka.hcb')

fails = []


def check(ok, msg):
    if ok:
        print('  ok   %s' % msg)
    else:
        print('  FAIL %s' % msg)
        fails.append(msg)


def main():
    if not os.path.isfile(SCAN):
        print('SKIP: scripts/scan-thread-calls.py missing')
        return 0
    if not os.path.isfile(HCB):
        print('SKIP: game data missing (%s)' % HCB)
        return 0
    tmp = os.path.join(ROOT, 'out', 'thread-calls.json')
    os.makedirs(os.path.dirname(tmp), exist_ok=True)
    rc = subprocess.call([sys.executable, SCAN, '--json', tmp])
    if rc != 0 or not os.path.isfile(tmp):
        print('FAIL: scanner exited %s / no json' % rc)
        return 1
    d = json.load(open(tmp, encoding='utf-8'))
    sites = d['sites']
    starts = [s for s in sites if s['name'] == 'ThreadStart']
    exits9 = sorted(s['pc'] for s in sites
                    if s['name'] == 'ThreadExit'
                    and any(p['name'] == 'pushint8' and p['args'] == [9]
                            for p in s['prev']))
    titles = sorted(s['pc'] for s in sites if s['name'] == 'TitleMenu')
    slots = set()
    for s in starts:
        for p in s['prev']:
            if p['name'] == 'pushint8':
                slots.add(p['args'][0])

    print('-- 1. ThreadStart 槽号 --')
    check(len(starts) == 35, 'ThreadStart 站点 35 个（实测 %d）' % len(starts))
    check(9 not in slots, '槽号不含 9（实测 %s）' % sorted(slots))

    print('-- 2. ThreadExit(9) 的四个站点 --')
    want = ['0x59e50', '0x59ec8', '0x59f09', '0x5a518']
    check(exits9 == want, 'ThreadExit(9) 站点 = %s（实测 %s）' % (want, exits9))

    # 说明：本工具只覆盖【由 call/jmp 目标当入口】解出来的指令；早期用字节模式扫到的 0x5A794 不在集合里。
    print('-- 3. TitleMenu(141) 站点 --')
    want_t = ['0x5a858', '0x5f5bb', '0x6133d', '0x61367', '0x64073']
    check(titles == want_t, 'TitleMenu 站点 = %s（实测 %s）' % (want_t, titles))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 线程/标题命令的调用点符合冻结事实（槽 9 从未启动）')
    return 0


if __name__ == '__main__':
    sys.exit(main())