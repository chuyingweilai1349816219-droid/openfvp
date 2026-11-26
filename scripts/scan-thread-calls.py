#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-thread-calls.py -- 静态列出 HCB 里 ThreadStart/ThreadExit/TitleMenu 的调用点。

动机（2026-11-26）：标题菜单跑在协程里（`ThreadExit(9)` 出现在
`0x59E50/0x59EC8/0x59F09/0x5A518`，`TitleMenu(141)` 在 `0x5A794/0x5A858`），
而 `prim 265` 的动作记录很可能是这条 slot 9 协程创建的。要查"这条协程的
`ThreadStart` 在哪、我们执行到没有"，就得有一份带上下文的静态清单。

做法：HCB 是代码+数据混排，不能整体线性反汇编。这里先用"`call`/`jmp`/`jmpcond`
的目标"当函数入口集合，再把每个入口线性解到 `ret`，得到一份可靠的指令边界集合；
最后在解出的指令里找 `syscall 132/133/136/141`，打印前 3 条指令作为实参上下文。

用法：
    python scripts/scan-thread-calls.py                     # 打印
    python scripts/scan-thread-calls.py --json out/thread.json
    python scripts/scan-thread-calls.py --game hoshimemo
"""
import argparse
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

WANT = {132: 'ThreadExit', 133: 'ThreadNext', 136: 'ThreadStart',
        141: 'TitleMenu'}


def entries(data):
    """call/jmp/jmpcond 的目标集合（粗筛，够用）。"""
    out = set()
    n = len(data)
    i = 0
    while i < n - 5:
        if data[i] in (0x02, 0x06, 0x07):
            out.add(struct.unpack_from('<I', data, i + 1)[0])
            i += 5
            continue
        i += 1
    return out


def decode_functions(code, entries_set):
    """从每个入口线性解码到 ret / 解不动为止，返回 {pc: (name, args, prev3)}。"""
    seen = {}
    for start in sorted(entries_set):
        if start in seen or start <= 0 or start >= code.script_len:
            continue
        pc = start
        prev = []
        for _ in range(4000):
            if pc in seen:
                break
            ins = code.at(pc)
            if ins is None:
                break
            op, name, args = ins
            seen[pc] = (name, args, list(prev[-3:]))
            prev.append((pc, name, args))
            pc += code.size[pc]
            if name in ('ret', 'ret2'):
                break
    return seen


def main(argv=None):
    ap = argparse.ArgumentParser(description='list thread/title calls in a HCB')
    ap.add_argument('--game', default='iroseka', choices=sorted(GAMES))
    ap.add_argument('--json', default='')
    a = ap.parse_args(argv)
    path = GAMES[a.game]
    if not os.path.isfile(path):
        print('SKIP: %s not found (%s)' % (a.game, path))
        return 0
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    ents = entries(data)
    seen = decode_functions(code, ents)
    sites = []
    for pc in sorted(seen):
        name, args, prev = seen[pc]
        if name != 'syscall' or not args:
            continue
        cid = args[0]
        if cid not in WANT:
            continue
        sites.append(dict(pc='0x%05x' % pc, cmd=cid, name=WANT[cid],
                          prev=[dict(pc='0x%05x' % p, name=n, args=ar)
                                for p, n, ar in prev]))
    print('%s: %d thread/title call site(s) (functions decoded: %d entries)'
          % (a.game, len(sites), len(ents)))
    for s in sites:
        ctx = ' | '.join('%s%s' % (p['name'], p['args'] if p['args'] else '')
                         for p in s['prev'])
        print('  %s %-11s prev: %s' % (s['pc'], s['name'], ctx))
    if a.json:
        with open(a.json, 'w', encoding='utf-8') as fp:
            json.dump(dict(game=a.game, count=len(sites), sites=sites), fp,
                      ensure_ascii=False, indent=1)
        print('json -> %s' % a.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())