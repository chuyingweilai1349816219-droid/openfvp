#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tailstat.py -- 对真实存档槽的「尾段（引擎状态快照）」做统计与差分。

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

用途：`docs/save-format.md` §2.4 / §4 里那段「引擎状态快照」只确定了总长度和几个
子结构，字段语义未解。这个脚本不猜语义，只回答**能从数据里直接量出来的问题**：

  * 每个槽的尾段有多长、其中多少字节非零；
  * 尾段头部 / 尾部若干字节在 6 个槽之间是否相同（相同 = 定长常量段）；
  * 同一位置的内容在槽间是否只差几个字节（说明该处是「少量枚举 + 大量零」）；
  * 尾段长度与哪一段（字符串数 / 缩略图）相关。

用法：
    python fvp/tools/tailstat.py <save 目录> [--thumb 200x113]
"""
from __future__ import annotations

import argparse
import io
import os
import sys

HEADER = 7


def read_u16(b, off):
    return b[off] | (b[off + 1] << 8)


def read_u32(b, off):
    return (b[off] | (b[off + 1] << 8) | (b[off + 2] << 16) |
            (b[off + 3] << 24))


def parse_slot(data, thumb_w, thumb_h):
    """返回 (时间戳, 3 条字符串, 尾段)。格式见 docs/save-format.md §2.1-§2.3。"""
    if len(data) < HEADER:
        raise ValueError('too short')
    ts = (read_u16(data, 0), data[2], data[3], data[4], data[5], data[6])
    pos = HEADER
    strs = []
    for _ in range(3):
        n = read_u16(data, pos)
        pos += 2
        strs.append(data[pos:pos + n])
        pos += n
    thumb_bytes = thumb_w * thumb_h * 4
    if pos + thumb_bytes > len(data):
        raise ValueError('thumbnail overruns the file')
    pos += thumb_bytes
    return ts, strs, data[pos:]


def nonzero(b):
    return sum(1 for x in b if x)


def common_prefix(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[i] == b[i]:
        i += 1
    return i


def common_suffix(a, b):
    n = min(len(a), len(b))
    i = 0
    while i < n and a[len(a) - 1 - i] == b[len(b) - 1 - i]:
        i += 1
    return i


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('save_dir')
    ap.add_argument('--thumb', default='200x113')
    args = ap.parse_args(argv)

    w, h = (int(x) for x in args.thumb.lower().split('x'))
    names = sorted(n for n in os.listdir(args.save_dir)
                   if len(n) == 8 and n.startswith('s') and n[3:].isdigit() is False
                   and n.endswith('.bin') and n[1:4].isdigit())
    if not names:
        print('tailstat: no sNNN.bin under %s' % args.save_dir, file=sys.stderr)
        return 2

    slots = []
    for name in names:
        path = os.path.join(args.save_dir, name)
        with io.open(path, 'rb') as f:
            data = f.read()
        ts, strs, tail = parse_slot(data, w, h)
        slots.append((name, ts, strs, tail))
        print('%-9s %6d B  tail %6d B  nonzero %6d  str %d/%d/%d' %
              (name, len(data), len(tail), nonzero(tail),
               len(strs[0]), len(strs[1]), len(strs[2])))

    print('\n== 尾段头 32 字节 ==')
    for name, ts, strs, tail in slots:
        print('%-9s %s' % (name, tail[:32].hex(' ')))

    print('\n== 尾段末 16 字节 ==')
    for name, ts, strs, tail in slots:
        print('%-9s %s' % (name, tail[-16:].hex(' ')))

    print('\n== 两两共同前缀 / 共同后缀（尾段） ==')
    print('%-10s %-10s %10s %10s' % ('A', 'B', 'prefix', 'suffix'))
    for i in range(len(slots)):
        for j in range(i + 1, len(slots)):
            a = slots[i][3]
            b = slots[j][3]
            print('%-10s %-10s %10d %10d' %
                  (slots[i][0], slots[j][0], common_prefix(a, b),
                   common_suffix(a, b)))

    print('\n== 结论提示 ==')
    print('  prefix > 0 说明尾段开头是定长常量段（可先按长度跳过）')
    print('  suffix > 0 说明尾段结尾是常量（docs 说末两字节 01 01）')
    print('  nonzero 远小于 tail 长度说明大段是零填充，字段稀疏')
    return 0


if __name__ == '__main__':
    sys.exit(main())