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

# 槽尾 128 槽动态字符串表的表头（见 docs/save-format.md §2.4.5 与 src/savetail.c）
# 偏移 = 0x163A0（32 个协程上下文）+ 每槽全局变量字节数；样本 = 0x163A0 + 0x3068。
POOL_OFFSET = 0x19408
POOL_TAG = 0x20      # 线程下标：0x20 = 存档时没有活跃脚本线程
POOL_MAX = 8         # 非空字符串条数上限


def parse_pool(tail):
    """按 src/savetail.c 的规则解析动态字符串表，返回 [(kind, text)] 或 []。

    规则：表头在 +0x19408 = u32 线程下标（样本 0x20）+ u32 非空条数（<=8），
    随后逐条 u16 长度 + NUL 结尾的 cp932 字符串，条目之间允许 0 填充（跳过去）。
    """
    if len(tail) < POOL_OFFSET + 8:
        return []
    if read_u32(tail, POOL_OFFSET) != POOL_TAG:
        return []
    count = read_u32(tail, POOL_OFFSET + 4)
    if count == 0 or count > POOL_MAX:
        return []
    off = POOL_OFFSET + 8
    out = []
    for _ in range(count):
        if off + 2 > len(tail):
            break
        n = read_u16(tail, off)
        off += 2
        if n < 2 or n > 160 or off + n > len(tail):
            break
        if tail[off + n - 1] != 0:
            break
        raw = tail[off:off + n - 1]
        try:
            text = raw.decode('cp932')
        except UnicodeDecodeError:
            text = repr(raw)
        out.append(text)
        off += n
        pad = 0
        while off < len(tail) and tail[off] == 0 and pad < 8:
            off += 1
            pad += 1
    return out


def looks_like_graph(s):
    """和 src/savetail.c 的判定一致：大写字母开头 + 含下划线。"""
    if not s or not ('A' <= s[0] <= 'Z'):
        return False
    if '_' not in s:
        return False
    letters = sum(1 for ch in s if 'A' <= ch <= 'Z')
    return letters >= 2



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


def runs_report(b, label, limit=24):
    """把一段字节按「全零段 / 非零段」切分，打印前 limit 段。"""
    out = []
    i = 0
    n = len(b)
    while i < n and len(out) < limit:
        j = i
        while j < n and b[j] == b[i] and (b[j] == 0) == (b[i] == 0):
            j += 1
        kind = 'ZERO' if b[i] == 0 else 'DATA'
        out.append((kind, i, j - i))
        i = j
    print('  %s: %d run(s) shown (len=%d)' % (label, len(out), n))
    for kind, off, ln in out:
        print('    +%-7d %-4s %6d' % (off, kind, ln))
    if i < n:
        print('    ... %d more bytes' % (n - i))


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

    print('\n== 槽尾动态字符串表 @+0x%X（src/savetail.c 的同一规则）==' % POOL_OFFSET)
    for name, ts, strs, tail in slots:
        pool = parse_pool(tail)
        if not pool:
            print('%-9s (none)' % name)
            continue
        graph = [x for x in pool if looks_like_graph(x)]
        print('%-9s %d string(s)%s' % (name, len(pool),
                                       ('  graph=%s' % graph[0]) if graph else ''))
        for k, text in enumerate(pool):
            tag = 'GRAPH' if looks_like_graph(text) else 'text '
            print('           [%d] %s %r' % (k, tag, text))

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

    # ---- 常量尾/常量头的内部结构（只在「所有槽都相同」的区间里做才有意义）----
    n_tail = len(slots)
    min_prefix = min(common_prefix(slots[i][3], slots[j][3])
                     for i in range(n_tail) for j in range(i + 1, n_tail))
    min_suffix = min(common_suffix(slots[i][3], slots[j][3])
                     for i in range(n_tail) for j in range(i + 1, n_tail))
    print('\n== 常量区（所有槽两两相同的部分）==')
    print('  常量头 %d B, 常量尾 %d B' % (min_prefix, min_suffix))

    if min_prefix > 0:
        heads = {s[3][:min_prefix] for s in slots}
        print('  常量头内部是否 6 槽全同: %s' % (len(heads) == 1))
        runs_report(slots[0][3][:min_prefix], '常量头分段')

    if min_suffix > 0:
        tails = {s[3][len(s[3]) - min_suffix:] for s in slots}
        print('  常量尾内部是否 6 槽全同: %s' % (len(tails) == 1))
        common = slots[0][3][len(slots[0][3]) - min_suffix:]
        print('  常量尾前 64 B: %s' % common[:64].hex(' '))
        runs_report(common, '常量尾分段（从头数）', limit=18)

    return 0


if __name__ == '__main__':
    sys.exit(main())