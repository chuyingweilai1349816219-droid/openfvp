#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""diff-text-baseline.py -- 把两份 `--text-out` 文本流逐行对齐，输出**结构化差异**。

为什么需要这个：`--faithful-ops --cmp-matrix` 已经把行尾 `」` 和行首 `~` 都修掉了，
但离冻结基线 `ref/ref-900k.txt` 还差 2 行。剩下的差异是"名牌 + 正文共用全局"这一类，
需要一个**稳定、可分类、可门禁**的表述，而不是每次靠肉眼 diff。

输出（`ref/text-delta-faithful.json`）：
  * summary：两边的行数、差异块数、各类差异的块数；
  * blocks：每块的 a/b 行范围 + 行内容 + 分类。

分类规则（只看文本形状，不做语义猜测）：
  quote     行里含 `」`（装饰多出来的引号）
  tilde     行里含 ASCII `~`（模板走错分支多出来的前缀）
  null-text 行形如 `[层号] 0` / `[层号] 1`（把 null/bool 当文本打出来）
  nameplate 层号 8 的行（名牌窗口）
  other     其余

用法：
    python scripts/diff-text-baseline.py --a out/a.txt --b ref/ref-900k.txt \
        --json ref/text-delta-faithful.json
    python scripts/diff-text-baseline.py --a ... --b ... --check
"""
import argparse
import difflib
import io
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_JSON = os.path.join(ROOT, 'ref', 'text-delta-faithful.json')

LINE_RE = re.compile(r'^\[(\d+)\] (.*)$')


def read_lines(path):
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        return [ln.rstrip('\r\n') for ln in fp]


def classify(lines):
    tags = set()
    for ln in lines:
        m = LINE_RE.match(ln)
        layer = int(m.group(1)) if m else None
        body = m.group(2) if m else ln
        if '」' in ln:
            tags.add('quote')
        if '~' in ln:
            tags.add('tilde')
        if body in ('0', '1'):
            tags.add('null-text')
        if layer == 8:
            tags.add('nameplate')
    if not tags:
        tags.add('other')
    return sorted(tags)


def build(a_path, b_path):
    a = read_lines(a_path)
    b = read_lines(b_path)
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    blocks = []
    counts = {}
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        left = a[i1:i2]
        right = b[j1:j2]
        cats = classify(left + right)
        for c in cats:
            counts[c] = counts.get(c, 0) + 1
        blocks.append({
            'kind': tag,
            'a': [i1, i2],
            'b': [j1, j2],
            'a_lines': left,
            'b_lines': right,
            'categories': cats,
        })
    return {
        'schema': 'fvp-text-delta/1',
        'a': os.path.basename(a_path),
        'b': os.path.basename(b_path),
        'summary': {
            'a_lines': len(a),
            'b_lines': len(b),
            'delta_lines': len(a) - len(b),
            'blocks': len(blocks),
            'by_category': counts,
        },
        'blocks': blocks,
        'note': '由 scripts/diff-text-baseline.py 生成；分类只看文本形状（quote/tilde/'
                'null-text/nameplate/other），不做语义猜测。',
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--a', required=True, help='新的文本流')
    ap.add_argument('--b', required=True, help='参照的文本流')
    ap.add_argument('--json', default=OUT_JSON)
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args(argv)

    data = build(args.a, args.b)
    text = json.dumps(data, ensure_ascii=False, indent=1) + '\n'

    if args.check:
        if not os.path.isfile(args.json):
            print('FAIL: %s 不存在' % args.json)
            return 1
        old = io.open(args.json, encoding='utf-8').read()
        if old != text:
            print('FAIL: %s 与当前两份文本流的差异不一致' % args.json)
            return 1
        print('PASS: 差异与基线一致（%d 块 / %d 类）'
              % (data['summary']['blocks'], len(data['summary']['by_category'])))
        return 0

    io.open(args.json, 'w', encoding='utf-8', newline='\n').write(text)
    if not args.quiet:
        s = data['summary']
        print('a=%d 行  b=%d 行  delta=%+d  差异块=%d'
              % (s['a_lines'], s['b_lines'], s['delta_lines'], s['blocks']))
        for k, v in sorted(s['by_category'].items()):
            print('  %-10s %d 块' % (k, v))
        for blk in data['blocks'][:8]:
            print('  %-7s a[%d:%d] b[%d:%d] %s'
                  % (blk['kind'], blk['a'][0], blk['a'][1],
                     blk['b'][0], blk['b'][1], ','.join(blk['categories'])))
    print('WROTE %s' % args.json)
    return 0


if __name__ == '__main__':
    sys.exit(main())