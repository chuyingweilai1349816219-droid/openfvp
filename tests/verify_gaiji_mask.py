#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""门禁：HZC1 格式 4（1-bit 掩码）必须解成"0 = 透明、1 = 白色字形"。

背景（2026-11-27 第 328 轮，用户现场报的"正文出现黑底水滴/汗/怒"）：

* `graph/gaiji_*` 这 12 个素材是 fmt=4：一字节一像素、值只有 0/1。
  `src/hzc1.c` 以前按"灰阶图"处理（`v*255` 且 alpha=255），于是整块
  156×156 的**黑底被当成不透明**贴到画面上 —— 就是那几块黑方块。
* 掩码语义没有歧义：0 表示"没有 ink"，1 表示字形。所以 0 → alpha 0，
  1 → 不透明的白。这条门禁用真实素材统计像素来钉它。

素材来源是本地的 `extract/graph/`（游戏数据派生，不入库）：目录不存在就 SKIP。
"""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.abspath(os.path.join(HERE, '..'))
ASSETS = os.path.join(FVP, 'extract', 'graph')
TOOL = os.path.join(FVP, 'bin', 'c_assets.exe')

# 覆盖三档尺寸各一个：156 / 234 / 102
SAMPLES = ('0714_gaiji_shiru', '0715_gaiji_shiru156', '0716_gaiji_shiru68')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def raw_mask(path):
    """自己解一遍：返回 (w, h, mask_bytes)，用来和 C 的解码结果对账。"""
    data = open(path, 'rb').read()
    if data[:4] != b'hzc1':
        return None
    fmt, w, h = struct.unpack_from('<HHH', data, 0x12)
    if fmt != 4:
        return None
    raw = zlib.decompress(data[0x2C:])
    return w, h, raw[:w * h]


def c_rgba(path):
    """用 bin/c_assets.exe rgba 导出 C 侧解码的 RGBA8888。"""
    fd, out = tempfile.mkstemp(suffix='.rgba')
    os.close(fd)
    proc = subprocess.run([TOOL, 'rgba', path, out],
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if proc.returncode != 0:
        os.unlink(out)
        return None, proc.stderr.decode('utf-8', 'replace')
    blob = open(out, 'rb').read()
    os.unlink(out)
    return blob, ''


def main():
    if not os.path.isdir(ASSETS):
        print('SKIP: 缺 %s（本地解包目录，不入库）' % ASSETS)
        return 0
    if not os.path.isfile(TOOL):
        print('SKIP: 缺 %s（先 mingw32-make -f Makefile test）' % TOOL)
        return 0

    for name in SAMPLES:
        path = os.path.join(ASSETS, name)
        if not os.path.isfile(path):
            print('SKIP: 缺素材 %s' % name)
            continue
        got = raw_mask(path)
        if got is None:
            check(False, '%s 不是 fmt=4' % name)
            continue
        w, h, mask = got
        ink = sum(1 for v in mask if v)
        print('== %s (%dx%d, ink=%d px) ==' % (name, w, h, ink))
        blob, err = c_rgba(path)
        if blob is None:
            check(False, '%s: c_assets rgba 失败：%s' % (name, err.strip()))
            continue
        check(len(blob) == w * h * 4,
              'C 侧 RGBA 长度 %d == %d' % (len(blob), w * h * 4))
        if len(blob) != w * h * 4:
            continue
        bad_alpha = 0
        bad_rgb = 0
        for i in range(w * h):
            r, g, b, a = blob[i * 4:i * 4 + 4]
            want = 255 if mask[i] else 0
            if a != want:
                bad_alpha += 1
            if r != want or g != want or b != want:
                bad_rgb += 1
        check(bad_alpha == 0, 'alpha 与掩码逐像素一致（不符 %d px）' % bad_alpha)
        check(bad_rgb == 0, '字形是白色、无 ink 处是黑（不符 %d px）' % bad_rgb)

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: fmt-4 掩码 = 0 透明 / 1 白色字形（黑底不再是不透明）')
    return 0


if __name__ == '__main__':
    sys.exit(main())