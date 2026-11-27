#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""trace-text-prints.py -- 把两种语义下的 `TextPrint` 逐条对齐，定位**第一处分歧**。

`FVP_DBG_TEXT=1` 会让 `TextPrint` 打一行：

    textprint pc=0x3a6d8 layer=0 g222=99 g226=0 t226=3 ... | <正文>

本脚本跑两遍（默认语义 / `--faithful-ops --cmp-matrix`），按**行号**对齐两份日志，
找出第一条内容不同的记录，连同前后几条一起冻结成 JSON。这样"差在哪一行、那一行
的脚本 pc 与相关全局是什么"就不用每次靠肉眼。

用法：
    python scripts/trace-text-prints.py                      # 打印 + 写 json
    python scripts/trace-text-prints.py --check               # 门禁用
"""
import argparse
import io
import json
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'ref', 'text-print-divergence.json')
FVM = os.path.join(ROOT, 'bin', 'fvm.exe')
HCB = r'C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb'
STEPS = '900000'

RE_LINE = re.compile(r'^textprint pc=0x([0-9a-f]+) layer=(-?\d+) '
                     r'g222=(-?\d+) g226=(-?\d+) '
                     r't226=(\d+) t233=(\d+) t231=(\d+) t225=(\d+) '
                     r't228=(\d+) t230=(\d+) \| (.*)$')


def run(extra):
    env = dict(os.environ)
    env['FVP_DBG_TEXT'] = '1'
    fd, out = tempfile.mkstemp(suffix='.txt')
    os.close(fd)
    try:
        cmd = [FVM, HCB, '--steps', STEPS, '--click', '600', '--quiet',
               '--text-out', out] + list(extra)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, timeout=900, env=env)
        recs = []
        for raw in proc.stderr.decode('utf-8', 'replace').splitlines():
            m = RE_LINE.match(raw)
            if not m:
                continue
            recs.append({
                'pc': '0x%s' % m.group(1),
                'layer': int(m.group(2)),
                'g222': int(m.group(3)),
                'g226': int(m.group(4)),
                'types': {
                    'g226': int(m.group(5)),
                    'g233': int(m.group(6)),
                    'g231': int(m.group(7)),
                    'g225': int(m.group(8)),
                    'g228': int(m.group(9)),
                    'g230': int(m.group(10)),
                },
                'text': m.group(11),
            })
        return recs
    finally:
        try:
            os.unlink(out)
        except OSError:
            pass


def build():
    if not (os.path.isfile(FVM) and os.path.isfile(HCB)):
        return None
    a = run([])
    b = run(['--faithful-ops', '--cmp-matrix'])
    n = min(len(a), len(b))
    first = None
    type_only = 0
    for i in range(n):
        same_content = (a[i]['pc'] == b[i]['pc'] and
                        a[i]['layer'] == b[i]['layer'] and
                        a[i]['text'] == b[i]['text'])
        if same_content:
            if a[i] != b[i]:
                type_only += 1
            continue
        first = i
        break
    if first is None:
        return {
            'schema': 'fvp-text-print-divergence/1',
            'steps': int(STEPS),
            'legacy_prints': len(a),
            'faithful_prints': len(b),
            'type_only_diffs': type_only,
            'first_divergence': None,
            'note': '两边的 TextPrint 日志在前 %d 条内正文完全一致'
                    '（另有 %d 条只是栈槽类型不同）。' % (n, type_only),
        }
    lo = max(0, first - 3)
    hi = min(n, first + 3)
    return {
        'schema': 'fvp-text-print-divergence/1',
        'steps': int(STEPS),
        'legacy_prints': len(a),
        'faithful_prints': len(b),
        'type_only_diffs': type_only,
        'first_divergence': {
            'index': first,
            'legacy': a[first],
            'faithful': b[first],
            'context_legacy': a[lo:hi],
            'context_faithful': b[lo:hi],
        },
        'note': '由 scripts/trace-text-prints.py 生成；两遍都开 FVP_DBG_TEXT，'
                '按行号对齐 TextPrint 日志。正文比较只看 (pc, layer, text)，'
                '只有栈槽类型不同的记录单独计数。',
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=OUT_JSON)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--quiet', action='store_true')
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
            print('FAIL: %s 与当前两遍日志不一致' % args.json)
            return 1
        print('PASS: 第一处分歧与基线一致')
        return 0

    io.open(args.json, 'w', encoding='utf-8', newline='\n').write(text)
    if not args.quiet:
        print('legacy TextPrint %d 条 / faithful %d 条'
              % (data['legacy_prints'], data['faithful_prints']))
        fd = data.get('first_divergence')
        if fd is None:
            print('前若干条完全一致')
        else:
            print('第一处分歧：第 %d 条' % fd['index'])
            print('  legacy  : %s' % json.dumps(fd['legacy'], ensure_ascii=False))
            print('  faithful: %s' % json.dumps(fd['faithful'], ensure_ascii=False))
    print('WROTE %s' % args.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())