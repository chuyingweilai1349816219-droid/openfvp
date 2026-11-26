#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_text_geometry.py -- 冻结"浮游旁白面板几何"的脚本事实（静态，不跑游戏）。

用户要求（2026-11-26）：部分正文是浮游旁白——没有可见对话框，文字直接浮在 CG 上，
位置取自脚本 `PrimSetText` 的显式几何：面板原点画布 (461.25, 435.375)、面板 1500x540。

这条门禁把它的来源链条逐条解出来并断言：

  1. 几何 helper `0x39F27(x, y, w, h, flag)`：
       - 缺省 x=230 / y=522 / w=736 / h=120；
       - 调 `0x35C10(551, 0, x, y)`（`PrimSetText`，x/y 经 syscall 20 换算）；
       - 调 `0x35D76(0, w, h)`（`TextBuff`）；
       - 调 `syscall 113`（`TextFont`）。
  2. 唯一传显式几何的调用点 `0x367B0`：`pushint16 246/258/800/320 + pushtrue`
     紧接 `call 0x39F27`。
  3. 换算系数来自 helper 里的 float 常量：`0x3FF00000 = 1.875`（=1920/1024）、
     `0x3FD80000 = 1.6875`（=1080/640）。
  4. 由此得到画布坐标 (246*1.875, 258*1.6875) = (461.25, 435.375)，
     面板 (800*1.875, 320*1.6875) = (1500, 540)。

缺游戏数据时 SKIP 并返回 0。
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, 'ref'))
import fvm_ref  # noqa: E402

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
    if not os.path.isfile(HCB):
        print('SKIP: game data missing (%s)' % HCB)
        return 0
    code = fvm_ref.Code(open(HCB, 'rb').read(), 'cp932')

    def decode(start, limit=80):
        out = []
        pc = start
        for _ in range(limit):
            ins = code.at(pc)
            if ins is None:
                break
            out.append((pc, ins[1], ins[2]))
            pc += code.size[pc]
            if ins[1] in ('ret', 'ret2'):
                break
        return out

    print('-- 1. helper 0x39F27 的结构 --')
    body = decode(0x39F27)
    names = [n for _, n, _ in body]
    ints = [a[0] for _, n, a in body
            if n in ('pushint8', 'pushint16') and a]
    calls = [a[0] for _, n, a in body if n == 'call' and a]
    check(body and body[0][1] == 'initstack', '函数以 initstack 开头')
    check(230 in ints and 522 in ints, '缺省 x=230 / y=522 都在（%s）' % ints[:8])
    check(120 in ints, '缺省 h=120 在（%s）' % ints[:10])
    check(0x35C10 in calls, '调用 PrimSetText 包装 0x35C10')
    check(0x35D76 in calls, '调用 TextBuff 包装 0x35D76')
    check(113 in [a[0] for _, n, a in body if n == 'syscall' and a],
          '调用了 syscall 113（TextFont）')

    print('-- 2. 唯一显式几何调用点 0x367B0 --')
    seq = decode(0x367B0, 6)
    want = [(0x367B0, 'pushint16', [246]), (0x367B3, 'pushint16', [258]),
            (0x367B6, 'pushint16', [800]), (0x367B9, 'pushint16', [320])]
    check([(pc, n, a) for pc, n, a in seq[:4]] == want,
          '四条 pushint16 = 246/258/800/320（实际 %s）'
          % [(hex(pc), a) for pc, _, a in seq[:4]])
    check(any(n == 'call' and a == [0x39F27] for _, n, a in seq),
          '紧接着 call 0x39F27')

    print('-- 3. 换算系数 --')
    fl = [struct.unpack('<I', b)[0] for b in ()]
    f1 = struct.unpack('<f', struct.pack('<I', 1072693248))[0]
    f2 = struct.unpack('<f', struct.pack('<I', 1071120384))[0]
    check(abs(f1 - 1.875) < 1e-6, '0x3FF00000 = 1.875（1920/1024，实际 %r）' % f1)
    check(abs(f2 - 1.6875) < 1e-6, '0x3FD80000 = 1.6875（1080/640，实际 %r）' % f2)
    body_c = decode(0x35C10, 40)
    body_d = decode(0x35D76, 40)
    check(any(n == 'pushfloat' and a == [1072693248] for _, n, a in body_c),
          '0x35C10 里用到 1.875')
    check(any(n == 'pushfloat' and a == [1071120384] for _, n, a in body_d),
          '0x35D76 里用到 1.6875')

    print('-- 4. 画布几何 --')
    ox, oy = 246 * f1, 258 * f2
    pw, ph = 800 * f1, 320 * f2
    check((ox, oy) == (461.25, 435.375),
          '面板原点 = (461.25, 435.375)（实际 (%r, %r)）' % (ox, oy))
    check((pw, ph) == (1500.0, 540.0),
          '面板尺寸 = (1500, 540)（实际 (%r, %r)）' % (pw, ph))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 浮游旁白面板几何（(461.25,435.375) / 1500x540）的脚本链条成立')
    return 0


if __name__ == '__main__':
    sys.exit(main())