#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""saveinfo.py -- FVP 引擎（FAVORITE VIEW POINT SYSTEM）存档解析器

支持两类文件:

  存档槽   <root>/save/sNNN.bin      (s481.bin, s800.bin, ...)
  全局设置 <root>/save/save.bin

用法:
    python saveinfo.py s481.bin                # 解析一个或多个槽文件，导出缩略图 PNG
    python saveinfo.py s481.bin s485.bin       # 批量
    python saveinfo.py --config save.bin       # 解析全局设置 save.bin
    python saveinfo.py --config save.bin -v    # 附带全局变量表统计

选项:
    --out DIR         PNG 输出目录（默认 fvp/out/save）
    --thumbsize WxH   强制缩略图尺寸（默认先读同目录 save.bin，再用像素统计推断）
    --map             打印缩略图之后的引擎状态段偏移（推定）
    --globals N       段内"每槽全局变量"条数，默认 0（见 docs/save-format.md）
    --no-png          不导出 PNG
    --json            以 JSON 输出槽信息

格式依据（详见 fvp/docs/save-format.md）:
  iretseka_HD.exe 0x43DE90  存档槽序列化函数（写）
  iretseka_HD.exe 0x43F0C7  存档槽反序列化（读）
  iretseka_HD.exe 0x4396F0  save.bin 序列化（写）
  iretseka_HD.exe 0x4392xx  save.bin 反序列化（读）
"""

from __future__ import annotations

import argparse
import json
import os
import struct
import sys

# ---------------------------------------------------------------------------
# 常量（全部来自静态分析）
# ---------------------------------------------------------------------------

DOW_NAMES = ["日", "月", "火", "水", "木", "金", "土"]

# save.bin 里记录缩略图尺寸（writer 0x43E0F9 用 engine+0xfd0 / +0xfd4）
CONFIG_TAIL_FLAG_BITS = 0x100          # FlagSet/FlagGet 位图 2048 bit
CONFIG_TAIL_READ_BITS = 0x100000       # 已读文本位图 8,388,608 bit
CONFIG_HEADER_LEN = 20                 # save.bin 的 20 字节固定头

# 单个文本窗口（engine+0x68dfc0 + i*0xb28）在线程快照里的定长部分
# 0x800 文本缓冲 + 8(0x814/0x818) + 4(0x808) + 4(0x80c) + 1(0x810)
# + 4(0x81c) + 4(0x820) + 4(0x824) + 0x100 + 0x200
CTX_SERIALIZED = 0x800 + 8 + 4 + 4 + 1 + 4 + 4 + 4 + 0x100 + 0x200
CTX_COUNT = 32

# 缺省缩略图：SaveThumbSize 的引擎初值 0x60 x 0x48；本作样本实测 200x113
DEFAULT_THUMB = (96, 72)
SAMPLE_THUMB = (200, 113)


# ---------------------------------------------------------------------------
# 字符串解码
# ---------------------------------------------------------------------------

def _score_text(s: str) -> int:
    """给解码结果打分：全角假名/汉字/中文标点高，半角片假名与私用区低。"""
    score = 0
    for c in s:
        o = ord(c)
        if 0x3040 <= o <= 0x30FF:          # 平假名/片假名
            score += 3
        elif 0x4E00 <= o <= 0x9FFF or 0x3400 <= o <= 0x4DBF:  # 汉字
            score += 3
        elif 0x3000 <= o <= 0x303F:        # 和文标点
            score += 2
        elif o == 0x20 or 32 <= o < 127:   # ASCII
            score += 1
        elif 0xFF01 <= o <= 0xFF60:        # 全角
            score += 1
        elif 0xFF61 <= o <= 0xFF9F:        # 半角片假名（多为误码）
            score -= 1
        elif 0xE000 <= o <= 0xF8FF:        # 私用区（几乎必为误码）
            score -= 5
        elif c.isprintable():
            score += 0
        else:
            score -= 3
    return score


def decode_text(raw: bytes) -> tuple[str, str]:
    """返回 (文本, 编码名)，在 CP932 与 GBK 之间取"更合理"的一个。

    FVP 用 ANSI 代码页：日文原版 CP932，本作汉化版脚本文本是 GBK。
    同一存档里章节名可能是 CP932、对话行可能是 GBK，所以逐串判定。
    """
    if not raw:
        return "", "empty"
    cands = []
    for enc in ("cp932", "gbk"):
        try:
            s = raw.decode(enc)
        except UnicodeDecodeError:
            continue
        cands.append((_score_text(s), enc, s))
    if not cands:
        return raw.decode("cp932", "replace"), "cp932/replace"
    cands.sort(key=lambda x: -x[0])
    _, enc, s = cands[0]
    return s, enc


def cstr_bytes(b: bytes) -> str:
    return b.split(b"\x00", 1)[0].decode("latin1")


# ---------------------------------------------------------------------------
# 存档槽
# ---------------------------------------------------------------------------

class SaveSlot:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 16:
            raise ValueError("文件过短")
        (
            self.year,
            self.month,
            self.day,
            self.dow,
            self.hour,
            self.minute,
        ) = struct.unpack_from("<HBBBBB", d, 0)
        self.strings: list[bytes] = []
        p = 7
        for _ in range(3):
            if p + 2 > len(d):
                raise ValueError("头部被截断")
            ln = struct.unpack_from("<h", d, p)[0]
            p += 2
            if ln < 0 or p + ln > len(d):
                raise ValueError("字符串长度非法: %d" % ln)
            self.strings.append(d[p:p + ln])
            p += ln
        self.thumb_offset = p

    # --- 元数据 ---------------------------------------------------------
    @property
    def timestamp(self):
        return (self.year, self.month, self.day, self.hour, self.minute)

    def timestamp_str(self):
        return "%04d-%02d-%02d (%s) %02d:%02d" % (
            self.year, self.month, self.day,
            DOW_NAMES[self.dow] if 0 <= self.dow < 7 else "?",
            self.hour, self.minute,
        )

    def mtime_str(self):
        st = os.stat(self.path)
        import datetime
        t = datetime.datetime.fromtimestamp(st.st_mtime)
        return t.strftime("%Y-%m-%d (%a) %H:%M:%S")

    def mtime_delta_minutes(self):
        import datetime
        st = os.stat(self.path)
        t = datetime.datetime.fromtimestamp(st.st_mtime)
        a = datetime.datetime(self.year, self.month, self.day, self.hour, self.minute)
        b = datetime.datetime(t.year, t.month, t.day, t.hour, t.minute)
        return int((b - a).total_seconds() // 60)

    # --- 缩略图 ---------------------------------------------------------
    def alpha_run(self) -> int:
        """从 thumb_offset 起，每隔 4 字节（alpha 通道）连续为 0xFF 的像素数。"""
        d = self.data
        i = self.thumb_offset + 3
        n = 0
        while i < len(d) and d[i] == 0xFF:
            n += 1
            i += 4
        return n

    def guess_thumb_size(self):
        """由 alpha 连续长度反推 (w, h)。

        缩略图是原始 BGRA，alpha 恒为 0xFF；图后紧跟的引擎状态数据 alpha 位置
        几乎不可能是 0xFF，所以连续长度 = w*h。分解时取 20<=w<=200、15<=h<=150
        且宽高比最接近 16:9 的解。
        """
        n = self.alpha_run()
        best = None
        for w in range(20, 201):
            if n % w:
                continue
            h = n // w
            if 15 <= h <= 150:
                score = abs(w / h - 16 / 9)
                if best is None or score < best[0]:
                    best = (score, w, h)
        if best:
            return best[1], best[2], "alpha-run"
        return None

    def thumb_pixels(self, size):
        w, h = size
        start = self.thumb_offset
        end = start + w * h * 4
        if end > len(self.data):
            return None
        return self.data[start:end]

    # --- 段映射（推定） -------------------------------------------------
    def section_map(self, thumb_size, globals_in_slot=0):
        """按 0x43DE90 写出顺序给出各段偏移。

        只有 header / 字符串 / 缩略图 是确证的；其后段边界依赖 HCB 里的
        "每槽全局变量数"，本作样本疑似 0。
        """
        d = self.data
        w, h = thumb_size
        off = self.thumb_offset
        rows = []
        rows.append(("缩略图 BGRA %dx%d" % (w, h), off, w * h * 4, "确证"))
        off += w * h * 4
        n = globals_in_slot
        rows.append(("每槽全局变量 %d×8B" % n, off, 8 * n, "推定"))
        off += 8 * n
        rows.append(("文本窗口快照 %d×0xB1D" % CTX_COUNT, off, CTX_COUNT * CTX_SERIALIZED, "推定"))
        off += CTX_COUNT * CTX_SERIALIZED
        rows.append(("当前文本窗口号 u32", off, 4, "推定"))
        off += 4
        # 动态字符串表：u32 计数 + 128 条 (i16 长度含 NUL + 字节)
        if off + 4 <= len(d):
            rows.append(("动态字符串表计数 u32", off, 4, "推定"))
            cnt = struct.unpack_from("<I", d, off)[0]
            rows.append(("  值 = %d" % cnt, off, 4, "推定"))
            off += 4
            lstart = off
            ok = True
            for _ in range(128):
                if off + 2 > len(d):
                    ok = False
                    break
                ln = struct.unpack_from("<h", d, off)[0]
                off += 2
                if ln < 0 or off + ln > len(d):
                    ok = False
                    break
                off += ln
            rows.append(("字符串表 128 槽", lstart, off - lstart, "推定" if ok else "解析失败"))
        rows.append(("引擎字节块 0x100×2", off, 0x200, "推定"))
        off += 0x200
        rows.append(("表计数 u32 + 256×20B", off, 4 + 256 * 20, "推定"))
        off += 4 + 256 * 20
        rows.append(("块 0x200×2", off, 0x400, "推定"))
        off += 0x400
        rows.append(("字节 + 16×12B", off, 1 + 16 * 12, "推定"))
        off += 1 + 16 * 12
        rows.append(("调色板 256×BGRA", off, 0x400, "推定"))
        off += 0x400
        rows.append(("u32×2", off, 8, "推定"))
        off += 8
        rows.append(("100×12B 表", off, 100 * 12, "推定"))
        off += 100 * 12
        rows.append(("（其后：图槽/动画/primitive 等变长段）", off, len(d) - off, "未解析"))
        return rows


# ---------------------------------------------------------------------------
# save.bin（全局设置）
# ---------------------------------------------------------------------------

class SaveConfig:
    """从文件尾部反向定位，不依赖任何计数。

    尾部结构（0x4396F0 写出顺序）:

        [全局变量数组 8×N] [20 字节头] [0x100000 已读位图]
        [0x100 Flag 位图] [u32] [i16 长度含 NUL + 字体名字节]

    其中 20 字节头:

        +0  u8   引擎 gfx+0x1fc 标志（与"全 CG 存档"一起变化）
        +1  u8   engine+0x80
        +2  u32  engine+0x48
        +6  u32  engine+0x4c
        +10 u8   engine+0x88
        +11 u8   engine+0x81
        +12 u32  缩略图宽
        +16 u32  缩略图高
    """

    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        n = len(d)

        # 1) 定位尾部字符串：找 L 使 "s[len]==L"
        self.font_len = None
        for L in range(2, 80):
            if n - L - 2 < 0:
                break
            if struct.unpack_from("<H", d, n - L - 2)[0] == L:
                self.font_len = L
                break
        if self.font_len is None:
            raise ValueError("无法定位尾部字符串（不是 FVP save.bin？）")
        L = self.font_len
        self.font_raw = d[n - L:n]
        self.tail_u32 = struct.unpack_from("<I", d, n - L - 2 - 4)[0]
        flag_off = n - L - 2 - 4 - CONFIG_TAIL_FLAG_BITS
        read_off = flag_off - CONFIG_TAIL_READ_BITS
        hdr_off = read_off - CONFIG_HEADER_LEN
        if hdr_off < 0:
            raise ValueError("文件过短，尾部结构与预期不符")
        self.flag_off = flag_off
        self.read_off = read_off
        self.hdr_off = hdr_off
        self.globals_bytes = hdr_off
        if hdr_off % 8:
            raise ValueError("全局变量区长度 %d 不是 8 的倍数" % hdr_off)
        self.globals_count = hdr_off // 8
        h = d[hdr_off:hdr_off + CONFIG_HEADER_LEN]
        self.hdr = h
        (self.gfx_1fc, self.e80) = h[0], h[1]
        self.e48, self.e4c = struct.unpack_from("<II", h, 2)
        self.e88, self.e81 = h[10], h[11]
        self.thumb_w, self.thumb_h = struct.unpack_from("<II", h, 12)

    # --- 全局变量表 -----------------------------------------------------
    def globals(self):
        d = self.data
        out = []
        for i in range(self.globals_count):
            o = i * 8
            t = d[o]
            v = struct.unpack_from("<I", d, o + 4)[0]
            out.append((t, v))
        return out

    def globals_type_histogram(self):
        hist = {}
        for t, _ in self.globals():
            hist[t] = hist.get(t, 0) + 1
        return dict(sorted(hist.items()))

    def flags(self):
        b = self.data[self.flag_off:self.flag_off + CONFIG_TAIL_FLAG_BITS]
        return [i for i in range(len(b) * 8) if b[i >> 3] >> (i & 7) & 1]

    def read_bits_count(self):
        b = self.data[self.read_off:self.read_off + CONFIG_TAIL_READ_BITS]
        return sum(bin(x).count("1") for x in b)

    def font_name(self):
        raw = self.font_raw[:-1] if self.font_raw.endswith(b"\x00") else self.font_raw
        s, _ = decode_text(raw)
        return s


# ---------------------------------------------------------------------------
# 输出
# ---------------------------------------------------------------------------

def png_export(slot: SaveSlot, size, outdir, verbose=True):
    try:
        from PIL import Image
    except ImportError:
        print("  ! 未安装 Pillow，跳过 PNG 导出", file=sys.stderr)
        return None
    px = slot.thumb_pixels(size)
    if px is None:
        print("  ! 缩略图超出文件长度", file=sys.stderr)
        return None
    os.makedirs(outdir, exist_ok=True)
    img = Image.frombytes("RGBA", size, px, "raw", "BGRA")
    base = os.path.splitext(os.path.basename(slot.path))[0]
    dst = os.path.join(outdir, base + "_thumb.png")
    img.save(dst)
    if verbose:
        print("  缩略图已导出: %s" % dst)
    return dst


def hexdump(data: bytes, off: int, n: int, label=""):
    if label:
        print("  -- %s --" % label)
    for i in range(0, n, 16):
        chunk = data[off + i:off + i + 16]
        if not chunk:
            break
        asc = "".join(chr(c) if 32 <= c < 127 else "." for c in chunk)
        print("  %08x  %-47s  |%s|" % (off + i, " ".join("%02x" % b for b in chunk), asc))


def config_hint_for(slot_path):
    """同目录 save.bin 里记录的缩略图尺寸。"""
    d = os.path.dirname(os.path.abspath(slot_path))
    cfg = os.path.join(d, "save.bin")
    if not os.path.isfile(cfg):
        return None
    try:
        c = SaveConfig(cfg)
        if 20 <= c.thumb_w <= 200 and 15 <= c.thumb_h <= 150:
            return (c.thumb_w, c.thumb_h), cfg
    except Exception:
        pass
    return None


def report_slot(path, args):
    slot = SaveSlot(path)
    print("=" * 72)
    print("文件      %s  (%d B, 0x%X)" % (path, len(slot.data), len(slot.data)))
    print("时间戳    %s        [格式: u16 年, u8 月/日/星期/时/分]" % slot.timestamp_str())
    print("mtime     %s" % slot.mtime_str())
    dm = slot.mtime_delta_minutes()
    if dm == 0:
        print("          -> 与文件修改时间一致（存档格式不含秒，精确到分）")
    else:
        print("          -> 与 mtime 相差 %d 分钟（可能是复制/打包改动了 mtime）" % dm)

    names = ["章节名/标题 (SaveCreate 0)", "字符串 1 (SaveCreate 1，本作未用)", "当前对话行 (SaveCreate 2)"]
    for raw, name in zip(slot.strings, names):
        s, enc = decode_text(raw)
        if raw:
            print("%-24s len=%-3d %-6s %r  (%s)" % (name, len(raw), enc, s, raw.hex()))
        else:
            print("%-24s (空)" % name)

    # 缩略图尺寸
    size = None
    src = None
    if args.thumbsize:
        if "x" in args.thumbsize.lower():
            w, h = args.thumbsize.lower().split("x")
            size, src = (int(w), int(h)), "--thumbsize"
    if size is None:
        hint = config_hint_for(path)
        if hint:
            size, src = hint[0], "save.bin:%s" % os.path.basename(hint[1])
    if size is None:
        g = slot.guess_thumb_size()
        if g:
            size, src = (g[0], g[1]), g[2]
    if size is None:
        size, src = SAMPLE_THUMB, "fallback"
    run = slot.alpha_run()
    print("缩略图    %dx%d = %d px = %d B  (来源: %s)" % (
        size[0], size[1], size[0] * size[1], size[0] * size[1] * 4, src))
    print("          像素直方图 alpha 连续长度 = %d (应等于 w*h)" % run)
    print("          数据偏移 = 0x%X" % slot.thumb_offset)

    out = None
    if not args.no_png:
        out = png_export(slot, size, args.out)

    if args.map:
        print("  段映射（缩略图之后，推定；确证部分只有前三段）:")
        for name, off, ln, tag in slot.section_map(size, args.globals):
            print("    0x%06X  %08d  %-30s %s" % (off, ln, name, tag))

    if args.json:
        print(json.dumps({
            "file": os.path.abspath(path),
            "size": len(slot.data),
            "timestamp": {
                "year": slot.year, "month": slot.month, "day": slot.day,
                "day_of_week": slot.dow, "hour": slot.hour, "minute": slot.minute,
                "text": slot.timestamp_str(),
            },
            "strings": [decode_text(r)[0] for r in slot.strings],
            "thumbnail": {"width": size[0], "height": size[1],
                          "offset": slot.thumb_offset},
            "png": out,
        }, ensure_ascii=False, indent=2))
    return slot, size, out


def report_config(path, args):
    c = SaveConfig(path)
    print("=" * 72)
    print("文件      %s  (%d B, 0x%X)" % (path, len(c.data), len(c.data)))
    print()
    print("段布局（自尾部反推，判据: 尾部 u16 长度 == 尾部字符串实际长度）:")
    print("  %-12s %-10s %-10s %s" % ("偏移", "长度", "结束", "内容"))
    rows = [
        (0, c.globals_bytes, "全局变量数组  %d × Value(8B)" % c.globals_count),
        (c.hdr_off, CONFIG_HEADER_LEN, "20 字节固定头"),
        (c.read_off, CONFIG_TAIL_READ_BITS, "已读文本位图 8388608 bit"),
        (c.flag_off, CONFIG_TAIL_FLAG_BITS, "FlagSet/FlagGet 位图 2048 bit"),
        (c.flag_off + CONFIG_TAIL_FLAG_BITS, 4, "u32 = %d" % c.tail_u32),
        (c.flag_off + CONFIG_TAIL_FLAG_BITS + 4, 2, "i16 字体名长度(含 NUL) = %d" % c.font_len),
        (c.flag_off + CONFIG_TAIL_FLAG_BITS + 6, c.font_len, "字体名 %r" % c.font_name()),
    ]
    for off, ln, name in rows:
        print("  0x%08X  %-10d 0x%08X %s" % (off, ln, off + ln, name))
    print()
    print("20 字节头 @0x%X:" % c.hdr_off)
    print("  +0   u8  gfx+0x1FC 标志      = %d" % c.gfx_1fc)
    print("  +1   u8  engine+0x80         = %d" % c.e80)
    print("  +2   u32 engine+0x48         = %d" % c.e48)
    print("  +6   u32 engine+0x4C         = %d" % c.e4c)
    print("  +10  u8  engine+0x88         = %d" % c.e88)
    print("  +11  u8  engine+0x81         = %d" % c.e81)
    print("  +12  u32 缩略图宽            = %d" % c.thumb_w)
    print("  +16  u32 缩略图高            = %d" % c.thumb_h)
    hexdump(c.data, c.hdr_off, CONFIG_HEADER_LEN, "hexdump(20B 头)")
    print()
    hist = c.globals_type_histogram()
    print("全局变量数组: %d 条 × 8 字节（u8 类型 + 3 字节填充 + u32 值）" % c.globals_count)
    print("  类型直方图 (0=未设/空, 1=?, 2=int): %s" % hist)
    flags = c.flags()
    print("Flag 位图: 已置位 bit = %s" % (flags if flags else "无（本作脚本从不调用 FlagSet）"))
    print("已读文本位图: 已置位 %d bit" % c.read_bits_count())
    print("尾 u32 = %d；字体名 = %r（%s）" % (c.tail_u32, c.font_name(), c.font_raw.hex()))
    if args.verbose:
        g = c.globals()
        nonnull = [(i, t, v) for i, (t, v) in enumerate(g) if t != 0]
        print()
        print("前 40 条全局变量（索引: 类型/值）:")
        for i, (t, v) in enumerate(g[:40]):
            print("  [%4d] type=%d value=%-10d %s" % (i, t, v, "" if t else "(未设)"))
        print("非空条目: %d / %d" % (len(nonnull), len(g)))
        print("类型 2 (int) 的前 30 个值: %s" % [v for t, v in g if t == 2][:30])


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(
        description="FVP 引擎存档解析器（存档槽 / save.bin 全局设置）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    ap.add_argument("files", nargs="*", help="槽文件 sNNN.bin，或配合 --config 的 save.bin")
    ap.add_argument("--config", action="store_true", help="把输入当作 save.bin 全局设置解析")
    ap.add_argument("--out", default=None, help="PNG 输出目录（默认 fvp/out/save）")
    ap.add_argument("--thumbsize", default=None, help="强制缩略图尺寸，如 200x113")
    ap.add_argument("--map", action="store_true", help="打印缩略图之后的段映射（推定）")
    ap.add_argument("--globals", type=int, default=0, metavar="N",
                    help="每槽全局变量条数（段映射用，默认 0）")
    ap.add_argument("--no-png", action="store_true", help="不导出 PNG")
    ap.add_argument("--json", action="store_true", help="槽信息以 JSON 输出")
    ap.add_argument("-v", "--verbose", action="store_true", help="详细输出（全局变量表）")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    if args.out is None:
        args.out = os.path.normpath(os.path.join(here, "..", "out", "save"))

    if not args.files:
        ap.print_help()
        return 1

    rc = 0
    for path in args.files:
        if not os.path.isfile(path):
            print("找不到文件: %s" % path, file=sys.stderr)
            rc = 2
            continue
        try:
            if args.config:
                report_config(path, args)
            else:
                report_slot(path, args)
        except Exception as e:
            print("解析失败 %s: %s" % (path, e), file=sys.stderr)
            rc = 2
    return rc


if __name__ == "__main__":
    sys.exit(main())