#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""scan-text-builder-calls.py -- 列出脚本里调用正文构建器的**全部**站点。

正文构建器是 `0x3A11F`（`initstack 4|0`），只能经由转发器 `0x3A2EE` 进入。
`FVP_DBG_CALL=1` 会在每次调用时打一行 `call 0x3A11F nargs=4 from=0x...`，
本脚本跑一遍、把 `from=` 去重，就得到脚本侧的真实调用点（静态扫字节会被数据段
里的同形字节骗到 5 万条，运行期日志不会）。

用法：
    python scripts/scan-text-builder-calls.py            # 打印 + 写 json
    python scripts/scan-text-builder-calls.py --check     # 门禁用
"""
import argparse
import io
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'ref', 'text-builder-calls.json')
FVM = os.path.join(ROOT, 'bin', 'fvm.exe')
HCB = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'
STEPS = '900000'
FORWARDER = '0x3a2f9'
RE_CALL = re.compile(r'^call 0x3A11F nargs=(\d+) from=0x([0-9a-f]+)$')


def build():
    if not (os.path.isfile(FVM) and os.path.isfile(HCB)):
        return None
    env = dict(os.environ)
    env['FVP_DBG_CALL'] = '1'
    proc = subprocess.run([FVM, HCB, '--steps', STEPS, '--click', '600',
                           '--quiet', '--faithful-ops', '--cmp-matrix',
                           '--text-out', os.devnull],
                          stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                          timeout=900, env=env)
    counts = {}
    total = 0
    for raw in proc.stderr.decode('utf-8', 'replace').splitlines():
        m = RE_CALL.match(raw)
        if not m:
            continue
        total += 1
        pc = '0x%05x' % int(m.group(2), 16)
        counts[pc] = counts.get(pc, 0) + 1
    # 转发器自己的那一条是内部调用，不算脚本站点
    forward = counts.pop(FORWARDER, 0)
    sites = sorted(counts)
    return {
        'schema': 'fvp-text-builder-calls/1',
        'steps': int(STEPS),
        'builder': '0x3a11f',
        'forwarder': '0x3a2ee',
        'argc': 4,
        'total_calls': total,
        'forwarder_calls': forward,
        'site_count': len(sites),
        'range': [sites[0], sites[-1]] if sites else None,
        'sites': sites,
        'note': '运行时日志（FVP_DBG_CALL=1）去重得到；静态扫字节会命中数据段里的'
                '同形序列，不可用。每条脚本调用都是 `pushstring <正文>; '
                'pushfalse ×3; call 0x3A2EE`。',
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=OUT_JSON)
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args(argv)

    data = build()
    if data is None:
        print('SKIP: bin/fvm.exe 或 iroseka.hcb 不存在')
        return 0
    text = json.dumps(data, ensure_ascii=False, indent=1) + '\n'

    if args.check:
        if not os.path.isfile(args.json):
            print('FAIL: %s 不存在' % args.json)
            return 1
        old = io.open(args.json, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与当前运行时日志不一致' % args.json)
            return 1
        print('PASS: 正文构建器的调用站点与基线一致（%d 个）' % data['site_count'])
        return 0

    io.open(args.json, 'w', encoding='utf-8', newline='\n').write(text)
    print('总调用 %d 次（其中转发器内部 %d 次），脚本站点 %d 个，范围 %s..%s'
          % (data['total_calls'], data['forwarder_calls'],
             data['site_count'], data['range'][0], data['range'][1]))
    print('WROTE %s' % args.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())