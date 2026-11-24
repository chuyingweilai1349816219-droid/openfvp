#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""make-font-atlas.py -- 为目标作品的 HCB 脚本生成 GFXF 位图字体图集。

桌面外壳（`bin\\fvp.exe`）用 `bin\\gfx-font-<size>.bin` 这类位图图集画字；
图集里没有的码点会退化成「豆腐块」（日志里的 `glyphs N tofu`）。早期图集是按
**iroseka 某一帧用到的 228 个字**生成的，所以跑《星空的记忆 HD》时开场就有 19 个
豆腐块。本脚本用 `ref/fvm_ref.py` 精确解出脚本里所有 `pushstring` 操作数（不是
"把二进制当 cp932 硬解"），收集两作真正用到的字符，再调用
`tests/gfx_dump.py::write_font()` 按与参考渲染器完全相同的栅格化参数
（同一 TTF、同一 margin、glyph/stroke1/stroke2 三层覆盖图）生成图集。

用法：
    python fvp/scripts/make-font-atlas.py \
        --hcb "C:\\...\\五彩斑斓的世界HD\\iroseka.hcb" \
        --hcb "C:\\...\\hoshimemo_installed\\Hoshimemo_HD.hcb" \
        --size 32 --size 34 --size 56 --size 30 \
        --out fvp/bin

只依赖 Python 3 + Pillow（DSH 运行时自带的那个 python 有 Pillow）。
退出码 0 = 生成成功，2 = 前置条件缺失（找不到 TTF / Pillow / 输入文件）。
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)

sys.path.insert(0, os.path.join(FVP, 'ref'))
sys.path.insert(0, os.path.join(FVP, 'tests'))


def collect_chars(hcb_path, enc):
    """返回该脚本里所有 pushstring 操作数覆盖的码点集合。"""
    import fvm_ref

    with open(hcb_path, 'rb') as f:
        data = f.read()
    code = fvm_ref.Code(data, enc)
    chars = set()
    for _pc, (op, name, args) in code.instr.items():
        if name != 'pushstring' or not args:
            continue
        for ch in args[0]:
            if ch in ('\r', '\n', '\t'):
                chars.add(' ')
                continue
            chars.add(ch)
    return chars


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--hcb', action='append', required=True,
                    help='要收集字符的 .hcb（可重复）')
    ap.add_argument('--enc', default='cp932',
                    help='pushstring 的文本编码（默认 cp932）')
    ap.add_argument('--size', action='append', type=int, required=True,
                    help='图集像素尺寸（可重复）；外壳按字号挑图集')
    ap.add_argument('--out', default=os.path.join(FVP, 'bin'),
                    help='输出目录（默认 fvp/bin）')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args(argv)

    try:
        from PIL import Image, ImageDraw  # noqa: F401
    except ImportError:
        sys.stderr.write('make-font-atlas: 需要 Pillow（用 DSH 运行时的 python）\n')
        return 2

    sys.path.insert(0, os.path.join(FVP, 'tests'))
    import gfx_dump

    chars = set()
    for p in args.hcb:
        if not os.path.isfile(p):
            sys.stderr.write('make-font-atlas: 找不到 %s\n' % p)
            return 2
        got = collect_chars(p, args.enc)
        chars |= got
        if not args.quiet:
            print('%-60s %5d 个字符' % (os.path.basename(p), len(got)))
    # 只丢掉控制字符（C0/C1）；全角空格 U+3000 这类 Separator 必须留下 ——
    # str.isprintable() 对 U+3000 返回 False，早期版本用它过滤，结果台词里的
    # 全角空格全变成豆腐块（实测 iroseka 1 个、hoshimemo 数个）。
    chars = set(c for c in chars if len(c) == 1 and ord(c) >= 0x20)
    chars.add('\u3000')
    chars.add('\u00a0')
    # ASCII 全量补上（界面/自检文本用），避免只有脚本里出现过的那几个英数字
    chars |= set(chr(c) for c in range(0x20, 0x7F))
    if not chars:
        sys.stderr.write('make-font-atlas: 没收集到任何字符\n')
        return 2
    if not args.quiet:
        print('并集 %d 个字符，尺寸 %s' % (len(chars), args.size))

    os.makedirs(args.out, exist_ok=True)
    for size in args.size:
        path = os.path.join(args.out, 'gfx-font-%d.bin' % size)
        metrics = gfx_dump.write_font(path, size, chars)
        if not args.quiet:
            print('%-40s %7d B  glyphs=%d ascent=%d descent=%d'
                  % (os.path.basename(path), os.path.getsize(path),
                     metrics['glyphs'], metrics['ascent'],
                     metrics['descent']))
    return 0


if __name__ == '__main__':
    sys.exit(main())