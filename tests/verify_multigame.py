#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多游戏兼容性门禁：三部 FVP 作品都要能跑、可重复、文本编码正确。

为什么要有这个：openbgi 之所以是"引擎项目"而不是"一个游戏的移植"，靠的是
一部一部的兼容性矩阵。openfvp 之前只在 iroseka 上做逐字节等价，第二部
《星空的记忆 HD》(Hoshimemo_HD.hcb) 只是"手工跑过一次"。这个脚本把它变成
可重复的门禁。

三部作品：`iroseka`（五彩斑斓的世界 HD）、`hoshimemo`（星空的记忆 HD）、
`sakura`（樱花，萌放。）。第三部是本轮新加的交叉验证对象：它的 HCB 里有
**和另外两部逐字节相同的 148 条命令表**（脚本里的 syscall 立即数就是这张表
的下标），但它的启动流程会在第 2179 步进到一个脚本自带的死循环
（pc 0x8ef82..0x8ef9b，清空 garr[1733] 的循环，循环条件 `i >= g[1711]`
在计数器为 0 时恒真）。引擎现在能用 `--stall-after` 侦测这种自旋，
所以这一部标成 **KNOWN BLOCKER**：不要求它跑出文本，只要求
① 卡死位置仍然是那个循环 ② 两次运行逐字节一致（说明是确定性卡死，不是
竞态）—— 一旦哪天它真的跑过去了，这条检查就会失败，提醒我们来改记录。

对每部作品断言：
  1. **能解析**：用 `ref/fvm_ref.py` 的 HCB 解码器解出指令，条数 > 100,000
  2. **能跑完**：`fvm <hcb> --steps 900000 --click 600` 退出码 0，不挂死
  3. **确定性**：同参数连跑两次，导出的文本**逐字节相同**
  4. **有产出**：文本行数 > 0；iroseka 必须正好 195 行（有冻结基准背书），
     其它作品只要求落在合理区间（>= 50）
  5. **编码正确**：输出里没有 U+FFFD（替换字符），日文假名/汉字占非空白字符
     的比例 > 50%（防止"跑通了但全是问号"这种假成功）

注意：这只是**兼容性冒烟 + 确定性**，不是逐字节等价。逐字节等价只有 iroseka
能做（有 Python 参考实现产出的 `ref/ref-900k.txt` 当基准）；Hoshimemo 没有
参考实现，所以只能证明"稳定、可重复、编码对"，不能证明"和原版一致"。

用法：
    python fvp/tests/verify_multigame.py
    python fvp/tests/verify_multigame.py --steps 200000   # 快速冒烟

退出码：0 = 通过，1 = 有断言失败，2 = 缺数据或二进制（SKIP）。
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
REF_DIR = os.path.join(FVP, 'ref')
OUT_DIR = os.path.join(FVP, 'out')
DOWNLOADS = r'C:\Users\Mechrevo\Downloads'

# 冻结基准对应 900k 步（ref/ref-900k.txt）。步数不同就不能拿它当行数判据，
# 否则 --fast 跑 300k 步会误报失败。
BASELINE_STEPS = 900000

# 名称, 脚本路径, 期望行数（None = 只要求 > min_lines）
GAMES = [
    dict(name='iroseka',  # 《五彩斑斓的世界 HD》
         dir=os.path.join(DOWNLOADS, '五彩斑斓的世界HD'),
         script=os.path.join(DOWNLOADS, '五彩斑斓的世界HD', 'iroseka.hcb'),
         expect_lines=195,
         # 标题菜单 profile（原版实测：右对齐 1769、行距 100、没有菜单贴图）
         title=dict(profile='iroseka', align='right', pitch=100,
                    right_edge=1769, textures=0,
                    continue_enabled=True, save_list=6,
                    bg='title_bg1_A',
                    overlays=['title_bg1_C', 'title_bg1_D',
                              'title_bg1_B'],
                    menu_navy=True, preview_slot=905,
                    logo='title_logo1', copyright='',
                    centres=[531, 631, 732, 831, 931])),
    dict(name='hoshimemo',  # 《星空的记忆 HD》
         dir=os.path.join(DOWNLOADS, 'hoshimemo_installed'),
         script=os.path.join(DOWNLOADS, 'hoshimemo_installed',
                             'Hoshimemo_HD.hcb'),
         expect_lines=None, min_lines=50,
         # 原版实测：居中 960、行距 90、5 张菜单贴图
         title=dict(profile='hoshimemo', align='centre', pitch=90,
                    centre_x=960, textures=5,
                    continue_enabled=False, save_list=0,
                    bg='title_bg1',
                    overlays=['title_frame2', 'title_frame'],
                    menu_navy=False, preview_slot=-1,
                    logo='title_logo1',
                    copyright='title_copyright',
                    centres=[597, 687, 777, 867, 957])),
    dict(name='sakura',  # 《樱花，萌放。（さくら、もゆ。）》
         dir=os.path.join(DOWNLOADS, '樱花，萌放（さくら、もゆ。）'),
         script=os.path.join(DOWNLOADS, '樱花，萌放（さくら、もゆ。）',
                             'Sakura.hcb'),
         expect_lines=None, min_lines=0,
         # 脚本自带的死循环（见文件头注释）：卡死位置必须落在这个窗口里
         known_blocker='启动流程在 pc 0x8ef82..0x8ef9b 的脚本循环里自旋'
                       '（计数器 global 1711 为 0 时 `i >= g[1711]` 恒真）',
         stall_window=(0x8ef82, 0x8ef9b)),
]

# 三部作品的 HCB 内嵌命令表必须一样（名字 + argc + 顺序）。
# 这是"跨作品兼容"最硬的一条不变量：脚本里的 syscall 立即数就是这张表的下标，
# 表一变，所有立即数的含义就全变了。
COMMAND_TABLE_SIZE = 148


def exe_path():
    base = os.path.join(FVP, 'bin', 'fvm')
    for cand in (base + '.exe', base):
        if os.path.isfile(cand):
            return cand
    return None


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def script_info(path):
    """用入库的参考解码器解一遍 HCB，返回 (script_len, instrs, main_start)。"""
    sys.path.insert(0, REF_DIR)
    try:
        import fvm_ref
    finally:
        sys.path.pop(0)
    data = open(path, 'rb').read()
    code = fvm_ref.Code(data, 'cp932')
    return code.script_len, len(code.instr), code.main_start


def cmd_table(path):
    """解析 HCB 尾部的内嵌命令表：[(argc, name), ...]。

    布局（三部作品一致）：u32 script_len | code | u32 main_start |
    7 字节头 | 标题(cp932, NUL) | u16 count | count x {u8 argc, u8 namelen
    (含 NUL), name} | u16 trailer(0)
    """
    import struct
    data = open(path, 'rb').read()
    script_len = struct.unpack_from('<I', data, 0)[0]
    off = script_len + 4 + 7
    off = data.index(b'\x00', off) + 1
    count = struct.unpack_from('<H', data, off)[0]
    off += 2
    out = []
    for _ in range(count):
        argc = data[off]
        namelen = data[off + 1]
        off += 2
        raw = data[off:off + namelen]
        off += namelen
        if raw.endswith(b'\x00'):
            raw = raw[:-1]
        out.append((argc, raw.decode('cp932', 'replace')))
    return out


def parse_stall_span(output):
    """从 fvm 汇总里取卡死的 pc 和无 syscall 步数：'stall pc=0x8ef96 after N'"""
    for line in output.splitlines():
        line = line.strip()
        if line.startswith('stall'):
            pc = None
            for tok in line.split():
                if tok.startswith('pc=0x'):
                    pc = int(tok[5:], 16)
            return pc
    return None


def run_once(exe, script, steps, out_path, stall_after=2000000):
    cmd = [exe, script, '--steps', str(steps), '--click', '600', '--quiet',
           '--stall-after', str(stall_after), '--stall-exit',
           '--text-out', out_path]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode('utf-8', 'replace')
    return p.returncode, text


def stall_hint(output):
    """意外出现 stall 时把 pc 带进失败信息里。"""
    pc = parse_stall_span(output)
    return '' if pc is None else ' -- stalled at pc 0x%x' % pc


# ---------------------------------------------------------------- 桌面外壳抓帧
# 控制台侧（fvm）证明"脚本能跑、文本对"；外壳侧证明"**画面**能出来"：
# 这两件事差着一整层渲染，第二部作品以前只被控制台验证过。外壳不参与逐字节
# 比对（墙钟驱动，帧数不确定），这里只断言"能出帧 + 没有豆腐块 + 没有未实现命令"。

SHELL_SECONDS = 8


def shell_path():
    base = os.path.join(FVP, 'bin', 'fvp')
    for cand in (base + '.exe', base):
        if os.path.isfile(cand):
            return cand
    return None


def have_font_atlas():
    b = os.path.join(FVP, 'bin')
    if not os.path.isdir(b):
        return False
    return any(fn.startswith('gfx-font-') and fn.endswith('.bin')
               for fn in os.listdir(b))


def run_shell_frame(exe, game_dir, script_name, seconds, shot, log_path,
                    extra=None):
    """跑外壳 N 秒并抓一帧。返回 (rc 或 None=超时, 日志文本)。"""
    # --no-input：忽略真实鼠标/键盘，截图只由程序状态决定（门禁确定性）
    cmd = [exe, '--no-input', '--game', game_dir,
           '--run-seconds', str(seconds), '--no-audio',
           '--screenshot', shot, '--log', log_path]
    if script_name:
        cmd += ['--script', script_name]
    if extra:
        cmd += list(extra)
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=seconds + 240)
    except subprocess.TimeoutExpired:
        return None, ''
    log = ''
    if os.path.isfile(log_path):
        log = open(log_path, encoding='utf-8', errors='replace').read()
    return p.returncode, log


def log_match(pattern, log):
    import re

    m = re.search(pattern, log, re.M)
    return m


def text_stats(text):
    """行数、U+FFFD 数、日文字符占比（假名 + 汉字）。"""
    lines = [ln for ln in text.splitlines() if ln.strip()]
    junk = text.count('\ufffd')
    total = 0
    jp = 0
    for ch in text:
        if ch.isspace():
            continue
        total += 1
        o = ord(ch)
        if 0x3040 <= o <= 0x30FF or 0x4E00 <= o <= 0x9FFF:
            jp += 1
    ratio = (jp / total) if total else 0.0
    return len(lines), junk, ratio


def read_bmp32(path):
    """读我们自己写的 32bpp BMP（bottom-up、BGRA、无 padding）。"""
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) < 54 or data[:2] != b'BM':
        return None
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    if w <= 0 or h <= 0:
        return None
    stride = w * 4
    rows = []
    for y in range(h):
        start = off + (h - 1 - y) * stride
        rows.append(data[start:start + stride])
    return w, h, rows


def ink_span(rows, y0, y1, x0=0, x1=1920, thresh=420):
    """在 y0..y1 的条带里找亮色墨迹。
    返回 (minx, maxx, miny, maxy, count)。"""
    minx = None
    maxx = None
    miny = None
    maxy = None
    count = 0
    h = len(rows)
    for y in range(max(0, y0), min(h, y1)):
        row = rows[y]
        for x in range(max(0, x0), min(len(row) // 4, x1)):
            b, g, r = row[x * 4], row[x * 4 + 1], row[x * 4 + 2]
            if r + g + b > thresh:
                count += 1
                if minx is None or x < minx:
                    minx = x
                if maxx is None or x > maxx:
                    maxx = x
                if miny is None:
                    miny = y
                maxy = y
    return minx, maxx, miny, maxy, count


def stage_title_menu(shell, games, check):
    """标题菜单：两部作品各跑一次 --title-menu，断言 profile、帖图/文字模式、
    以及画出来的菜单墨迹几何（右边界 / 中心 / 行距）与原版实测一致。"""
    for g in games:
        t = g.get('title')
        if not t or not g.get('dir'):
            continue
        name = g['name']
        print('== %s title menu (--title-menu) ==' % name)
        shot = os.path.join(OUT_DIR, 'mg-%s-title.bmp' % name)
        log_path = os.path.join(OUT_DIR, 'mg-%s-title.log' % name)
        for p in (shot, log_path):
            if os.path.isfile(p):
                os.remove(p)
        rc, log = run_shell_frame(shell, g['dir'],
                                  os.path.basename(g['script']), 2, shot,
                                  log_path,
                                  extra=['--title-menu',
                                         '--title-no-bg'])
        check(rc == 0, 'title menu shell exit 0 (got %s)' % rc)
        m = log_match(r'^title menu\s+enabled \(profile=([a-z]+), '
                      r'align=(\w+),', log)
        check(m is not None, 'title menu summary line (%s)'
              % (m.group(0) if m else 'missing'))
        if m is not None:
            check(m.group(1) == t['profile'],
                  'profile is %s (got %s)' % (t['profile'], m.group(1)))
            check(m.group(2) == t['align'],
                  'align is %s (got %s)' % (t['align'], m.group(2)))
        want = 'enabled' if t['continue_enabled'] else 'disabled'
        check(('continue=%s' % want) in log,
              'continue is %s in the title summary (save slots under save/)'
              % want)
        if t['textures'] > 0:
            m = log_match(r'title: (\d+)/5 menu textures loaded', log)
            check(m is not None and int(m.group(1)) == t['textures'],
                  '%d menu texture(s) loaded (%s)'
                  % (t['textures'], m.group(0) if m else 'missing'))
        else:
            check('falling back to text' in log,
                  'falls back to text when the title has no menu textures')
        # 单独跑一次"带背景"的版本：背景断言和 hover 对照都以它为基准
        shot_bg = os.path.join(OUT_DIR, 'mg-%s-bg.bmp' % name)
        log_bg_path = os.path.join(OUT_DIR, 'mg-%s-bg.log' % name)
        for pb in (shot_bg, log_bg_path):
            if os.path.isfile(pb):
                os.remove(pb)
        rc_bg, log_bg = run_shell_frame(shell, g['dir'],
                                        os.path.basename(g['script']), 2,
                                        shot_bg, log_bg_path,
                                        extra=['--title-menu'])
        check(rc_bg == 0, 'background run exit 0 (got %s)' % rc_bg)

        # 背景叠加层：iroseka 的风车/花瓣/光束在 B/C/D 里，星空的"整体提亮"
        # 在 title_frame2 里；都要真的装上
        for ov in t.get('overlays') or []:
            check(("bg overlay '%s' loaded" % ov) in log_bg,
                  'background overlay %s loaded' % ov)

        # logo / 版权行：名字来自 profile，位置来自素材头里的 x/y
        if t.get('logo'):
            m = log_match(r"title: logo '([^']+)' loaded", log_bg)
            check(m is not None and m.group(1) == t['logo'],
                  'title logo %s loaded (%s)'
                  % (t['logo'], m.group(0) if m else 'missing'))
        if t.get('copyright'):
            m = log_match(r"title: copyright '([^']+)' loaded", log_bg)
            check(m is not None and m.group(1) == t['copyright'],
                  'title copyright %s loaded (%s)'
                  % (t['copyright'], m.group(0) if m else 'missing'))


        # 标题背景：有 bg 素材的作品要真的铺上（帧不再是黑的）
        if t.get('bg'):
            m = log_match(r"title: background '([^']+)' loaded",
                          log_bg)
            check(m is not None and m.group(1) == t['bg'],
                  'title background %s loaded (%s)'
                  % (t['bg'], m.group(0) if m else 'missing'))
        img_geom = read_bmp32(shot) if os.path.isfile(shot) else None
        img_bg = read_bmp32(shot_bg) if os.path.isfile(shot_bg) else None
        check(img_geom is not None and img_bg is not None,
              'title frame BMPs readable')
        if img_geom is None or img_bg is None:
            print('')
            continue
        # 几何量在 --title-no-bg 那一帧上做（背景亮部会污染墨迹检测）；
        # 亮度/背景断言用带背景那一帧。
        w, h, rows = img_geom
        check(w == 1920 and h == 1080, 'frame is 1920x1080 (got %dx%d)'
              % (w, h))
        if t.get('menu_navy'):
            # 第一项墨迹带里最暗的像素应该是深蓝（实测原版 RGB(0,19,143)）
            dark = []
            for yy in range(t['centres'][0] - 30, t['centres'][0] + 35):
                row = img_bg[2][yy]
                for xx in range(1440, 1830):
                    px = row[xx * 4:xx * 4 + 3]
                    dark.append((px[0] + px[1] + px[2], px))
            dark.sort(key=lambda item: item[0])
            pick = dark[:300]
            if pick:
                # BMP is BGRA: px[0]=B, px[1]=G, px[2]=R
                bb = sum(p[1][0] for p in pick) / len(pick)
                gg = sum(p[1][1] for p in pick) / len(pick)
                rr = sum(p[1][2] for p in pick) / len(pick)
                check(bb > 90 and bb > rr + 60,
                      'text menu uses the measured navy colour '
                      '(mean BGR %.0f,%.0f,%.0f)' % (bb, gg, rr))
        # 背景铺上后整帧不再是黑的；没有背景素材的作品仍是文字菜单（黑底）
        bright = 0
        rows_bg = img_bg[2]
        for yy in range(0, h, 16):
            row = rows_bg[yy]
            for xx in range(0, w, 16):
                if (row[xx * 4] + row[xx * 4 + 1] + row[xx * 4 + 2]) > 90:
                    bright += 1
        total = (h // 16 + 1) * (w // 16 + 1)
        if t.get('bg'):
            check(bright > total // 2,
                  'title frame is mostly lit by the background (%d of %d)'
                  % (bright, total))
        else:
            # 没有背景素材的作品：只有 logo（和文字菜单）是亮的，整帧仍是深色
            check(bright < total // 3,
                  'title frame stays mostly dark without a background asset '
                  '(only the logo lights up: %d of %d)' % (bright, total))
        # Text-mode menus use dark navy glyphs, so on the black
        # --title-no-bg frame the ink is the DARK pixels; texture menus
        # are white on black, so they need the bright threshold.
        ink_thresh = 24 if t.get('menu_navy') else 420
        got_centres = []
        for i, cy in enumerate(t['centres']):
            minx, maxx, miny, maxy, cnt = ink_span(rows, cy - 34, cy + 40,
                                                  thresh=ink_thresh)
            check(cnt > 40, 'row %d has ink (%d px)' % (i, cnt))
            if minx is None:
                got_centres.append(None)
                continue
            # 行距用**竖直中心**算（横向中心是对齐基准，不能当行距）
            got_centres.append((miny + maxy) // 2)
            if t['align'] == 'right':
                check(abs(maxx - t['right_edge']) <= 8,
                      'row %d ink right %d ~ %d' % (i, maxx,
                                                   t['right_edge']))
            else:
                c = (minx + maxx) // 2
                check(abs(c - t['centre_x']) <= 10,
                      'row %d centred at %d ~ %d' % (i, c, t['centre_x']))
        for i in range(1, len(got_centres)):
            a, b = got_centres[i - 1], got_centres[i]
            if a is None or b is None:
                continue
            check(abs((b - a) - t['pitch']) <= 6,
                  'row pitch %d -> %d = %d ~ %d'
                  % (i - 1, i, b - a, t['pitch']))
        # 第二次跑：直接打开 Continue 的存档列表（--title-save-list），
        # 只读：断言可读槽数 = 期望值。
        shot2 = os.path.join(OUT_DIR, 'mg-%s-savelist.bmp' % name)
        log2_path = os.path.join(OUT_DIR, 'mg-%s-savelist.log' % name)
        for p2 in (shot2, log2_path):
            if os.path.isfile(p2):
                os.remove(p2)
        rc2, log2 = run_shell_frame(shell, g['dir'],
                                    os.path.basename(g['script']), 2, shot2,
                                    log2_path,
                                    extra=['--title-menu', '--title-save-list'])
        check(rc2 == 0, 'save list shell exit 0 (got %s)' % rc2)
        m = log_match(r'save list: (\d+) slot\(s\) readable', log2)
        check(m is not None and int(m.group(1)) == t['save_list'],
              '%d save slot(s) readable (%s)'
              % (t['save_list'], m.group(0) if m else 'missing'))
        if t['save_list'] > 0:
            m = log_match(r'save list: first=No\.\d+\s+\d{4}/\d{2}/\d{2}',
                          log2)
            check(m is not None,
                  'first save-list row has a slot number and a timestamp (%s)'
                  % ((m.group(0) if m else 'missing')))
        m = log_match(r'continue=\w+ \(\d+ slot\(s\), (\d+) readable\)',
                      log2)
        check(m is not None and int(m.group(1)) == t['save_list'],
              'summary reports %d readable slot(s) (%s)'
              % (t['save_list'], m.group(0) if m else 'missing'))
        print('')

        # 第三次跑：--title-hover 1。hover 态必须真的画出东西（贴图模式 = 发光层，
        # 文字模式 = 高亮色）；和第一次的截图逐像素比，差异要落在被 hover 的那一行。
        shot3 = os.path.join(OUT_DIR, 'mg-%s-hover.bmp' % name)
        log3_path = os.path.join(OUT_DIR, 'mg-%s-hover.log' % name)
        for p3 in (shot3, log3_path):
            if os.path.isfile(p3):
                os.remove(p3)
        rc3, log3 = run_shell_frame(shell, g['dir'],
                                    os.path.basename(g['script']), 2, shot3,
                                    log3_path,
                                    extra=['--title-menu', '--title-hover', '1'])
        check(rc3 == 0, 'hover run exit 0 (got %s)' % rc3)
        img_a = read_bmp32(shot_bg) if os.path.isfile(shot_bg) else None
        img_b = read_bmp32(shot3) if os.path.isfile(shot3) else None
        if img_a is not None and img_b is not None:
            diff = 0
            band = 0
            band_y0 = t['centres'][1] - 40
            band_y1 = t['centres'][1] + 50
            for yy in range(img_a[1]):
                ra = img_a[2][yy]
                rb = img_b[2][yy]
                for xx in range(img_a[0]):
                    if ra[xx * 4:xx * 4 + 3] != rb[xx * 4:xx * 4 + 3]:
                        diff += 1
                        if band_y0 <= yy <= band_y1:
                            band += 1
            check(diff > 200, 'hover state changes pixels (%d)' % diff)
            if t['textures'] > 0:
                # 贴图模式：hover = 整行发光层，差异必须全落在被 hover 的那一行
                check(band == diff,
                      'all hover pixels are in the hovered row band (%d of %d)'
                      % (band, diff))
            else:
                # 文字模式：hover 只是高亮色，只断言“真的变了”
                check(band > 0,
                      'hovered row itself changed (%d of %d)' % (band, diff))
        print('')


def stage_opening(shell, games, check):
    """开场序列：白底 → logo 淡入 → 白场 → 标题。用 --shot-at 抓三个时刻。"""
    g = None
    for cand in games:
        if cand.get('dir') and not cand.get('known_blocker'):
            g = cand
            break
    if g is None:
        return
    name = g['name']
    print('== %s opening sequence (--opening) ==' % name)
    moments = [(200, 'white hold'), (1500, 'logo shown'), (6200, 'title')]
    shots = {}
    for ms, tag in moments:
        shot = os.path.join(OUT_DIR, 'mg-%s-open-%d.bmp' % (name, ms))
        log_path = os.path.join(OUT_DIR, 'mg-%s-open-%d.log' % (name, ms))
        for p2 in (shot, log_path):
            if os.path.isfile(p2):
                os.remove(p2)
        rc, log = run_shell_frame(shell, g['dir'],
                                 os.path.basename(g['script']), 7, shot,
                                 log_path,
                                 extra=['--opening', '--title-menu',
                                        '--shot-at', str(ms)])
        check(rc == 0, 'opening run @%d ms exit 0 (got %s)' % (ms, rc))
        check('opening: logo_bg + logo_favo loaded' in log,
              'opening assets loaded @%d ms' % ms)
        check("shot-at" in log, 'mid-run screenshot taken @%d ms' % ms)
        img = read_bmp32(shot) if os.path.isfile(shot) else None
        check(img is not None, 'opening frame @%d ms readable' % ms)
        if img is None:
            continue
        rows = img[2]
        total = 0
        logo = 0
        for yy in range(0, 1080, 8):
            row = rows[yy]
            for xx in range(0, 1920, 8):
                v = row[xx * 4] + row[xx * 4 + 1] + row[xx * 4 + 2]
                total += 1
                if 400 <= xx <= 1400 and 380 <= yy <= 560:
                    logo += 1 if v < 720 else 0
        shots[ms] = (total, logo)
    if 200 in shots:
        check(shots[200][1] == 0,
              'white hold @200 ms has no logo pixels (%d)' % shots[200][1])
    if 1500 in shots:
        check(shots[1500][1] > 100,
              'logo visible @1500 ms (%d dark samples in the logo box)'
              % shots[1500][1])
    check('opening: finished' in log, 'opening reports completion')

    # New Game 的白场过渡：淡到白 → 白场峰值启动脚本 → 淡出
    moments = [(450, 'white peak'), (1600, 'script running')]
    means = {}
    for ms, tag in moments:
        shot = os.path.join(OUT_DIR, 'mg-%s-start-%d.bmp' % (name, ms))
        log_path = os.path.join(OUT_DIR, 'mg-%s-start-%d.log' % (name, ms))
        for p2 in (shot, log_path):
            if os.path.isfile(p2):
                os.remove(p2)
        rc, lg = run_shell_frame(shell, g['dir'],
                                 os.path.basename(g['script']), 3, shot,
                                 log_path,
                                 extra=['--title-menu', '--pick', '0',
                                        '--shot-at', str(ms)])
        check(rc == 0, 'start transition run @%d ms exit 0 (got %s)'
              % (ms, rc))
        img = read_bmp32(shot) if os.path.isfile(shot) else None
        if img is not None:
            rows = img[2]
            total = 0
            lum = 0
            for yy in range(0, 1080, 16):
                row = rows[yy]
                for xx in range(0, 1920, 16):
                    lum += row[xx * 4] + row[xx * 4 + 1] + row[xx * 4 + 2]
                    total += 1
            means[ms] = lum / (3.0 * total)
        if ms == 450:
            check('title: start transition (800 ms, white)' in lg,
                  'start transition logged')
            check(means.get(450, 0) > 200.0,
                  'frame is near white at the peak (mean %.0f)'
                  % means.get(450, -1))
        else:
            check('vm: started at pc=0x' in lg,
                  'script started during the transition (%s)'
                  % ('yes' if 'vm: started at pc=0x' in lg else 'no'))
            check(means.get(1600, 999) < 60.0,
                  'frame left the white peak and shows the script (mean %.0f)'
                  % means.get(1600, -1))

    print('')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=900000)
    ap.add_argument('--no-shell', action='store_true',
                    help='跳过桌面外壳抓帧阶段（无 GUI 的环境用）')
    args = ap.parse_args()

    exe = exe_path()
    if exe is None:
        print('verify_multigame: bin/fvm(.exe) missing (run: make)')
        return 2
    have = [g for g in GAMES if os.path.isfile(g['script'])]
    missing = [g for g in GAMES if not os.path.isfile(g['script'])]
    for g in missing:
        print('verify_multigame: missing %s (%s)' % (g['name'], g['script']))
    if not have:
        print('verify_multigame: SKIPPED -- no game data')
        return 2
    if not os.path.isdir(OUT_DIR):
        os.makedirs(OUT_DIR)

    fails = []

    # 0. 跨作品不变量：三部作品的内嵌命令表必须逐项相同。
    tables = {}
    for g in have:
        try:
            tables[g['name']] = cmd_table(g['script'])
        except Exception as exc:  # noqa: BLE001
            print('  FAIL  %s command table parses (%s)' % (g['name'], exc))
            fails.append('%s command table' % g['name'])
    if len(tables) == len(have) and tables:
        first = have[0]['name']
        base = tables[first]
        for name, tbl in tables.items():
            same = tbl == base
            print('  %-4s  command table of %-9s == %s (%d entries)'
                  % ('ok' if same else 'FAIL', name, first, len(tbl)))
            if not same:
                fails.append('%s command table differs' % name)
        print('  %-4s  every title declares %d commands'
              % ('ok' if len(base) == COMMAND_TABLE_SIZE else 'FAIL',
                 len(base)))
        if len(base) != COMMAND_TABLE_SIZE:
            fails.append('command table size')

    def check(cond, msg):
        print('  %s  %s' % ('ok  ' if cond else 'FAIL', msg))
        if not cond:
            fails.append(msg)

    for g in have:
        name = g['name']
        print('== %s @ %d steps ==' % (name, args.steps))

        # 1. 能解析
        try:
            script_len, instrs, main_start = script_info(g['script'])
        except Exception as exc:  # noqa: BLE001 - 解码失败就是要报出来
            check(False, 'HCB decodes (%s)' % exc)
            script_len = instrs = main_start = 0
            continue
        check(script_len > 1_000_000, 'script_len %d > 1 MB' % script_len)
        check(instrs > 100_000, '%d instructions decoded' % instrs)
        check(0 < main_start < script_len,
              'main_start 0x%x inside the script' % main_start)

        # 2. 两次跑，4. 确定性
        a_path = os.path.join(OUT_DIR, 'mg-%s-a.txt' % name)
        b_path = os.path.join(OUT_DIR, 'mg-%s-b.txt' % name)
        # 已知卡死的作品要在 --steps 预算之内就能判定，所以阈值调小
        stall_after = 50000 if g.get('known_blocker') else 2000000
        rc_a, out_a = run_once(exe, g['script'], args.steps, a_path,
                               stall_after)
        rc_b, out_b = run_once(exe, g['script'], args.steps, b_path,
                               stall_after)
        if g.get('known_blocker'):
            lo, hi = g['stall_window']
            pc = parse_stall_span(out_a)
            check(rc_a == 6 and rc_b == 6,
                  'both runs stop with the stall exit code 6 (%d/%d)'
                  % (rc_a, rc_b))
            check(pc is not None and lo <= pc <= hi,
                  'still stalls inside 0x%x..0x%x (pc %s)'
                  % (lo, hi, ('0x%x' % pc) if pc is not None else 'none'))
        else:
            check(rc_a == 0, 'run A exit 0 (got %d)%s' % (rc_a, stall_hint(out_a)))
            check(rc_b == 0, 'run B exit 0 (got %d)%s' % (rc_b, stall_hint(out_b)))
        ok_files = os.path.isfile(a_path) and os.path.isfile(b_path)
        check(ok_files, 'both text dumps written')
        if ok_files:
            same = sha256(a_path) == sha256(b_path)
            check(same, 'two runs are byte-identical (deterministic)')
            text = open(a_path, encoding='utf-8', errors='replace').read()
            lines, junk, ratio = text_stats(text)
            # 3. 有产出（行数判据只对 900k 步成立：步数变了行数自然变）
            if g.get('known_blocker'):
                pass  # 已知卡死：0 行是预期结果，见文件头注释
            elif args.steps == BASELINE_STEPS:
                if g['expect_lines'] is not None:
                    check(lines == g['expect_lines'],
                          'text lines == %d (frozen baseline)'
                          % g['expect_lines'])
                else:
                    check(lines >= g.get('min_lines', 50),
                          'text lines %d >= %d'
                          % (lines, g.get('min_lines', 50)))
            else:
                check(lines > 0, 'text lines %d > 0 (smoke step count)' % lines)
            # 5. 编码正确
            check(junk == 0, 'no U+FFFD replacement chars (got %d)' % junk)
            if not g.get('known_blocker'):
                check(ratio > 0.5,
                      'japanese chars %.1f%% of non-space text'
                      % (ratio * 100.0))
            print('     (%d lines, %d bytes, jp %.1f%%)'
                  % (lines, os.path.getsize(a_path), ratio * 100.0))
        if g.get('known_blocker'):
            print('     KNOWN BLOCKER: %s' % g['known_blocker'])
        print('')

    # 6. 桌面外壳抓帧：两部目标作品都要能出"可读"的一帧。
    if args.no_shell:
        print('  skip  desktop shell stage (--no-shell)')
    else:
        shell = shell_path()
        if shell is None:
            print('  skip  desktop shell not built (bin/fvp.exe)')
        else:
            atlas = have_font_atlas()
            if not atlas:
                print('  note  bin/gfx-font-*.bin 不存在：跳过 tofu 断言'
                      '（先跑 scripts/make-font-atlas.py）')
            for g in have:
                if g.get('known_blocker') or not g.get('dir'):
                    continue
                name = g['name']
                print('== %s desktop shell @ %d s ==' % (name, SHELL_SECONDS))
                shot = os.path.join(OUT_DIR, 'mg-%s-frame.bmp' % name)
                log_path = os.path.join(OUT_DIR, 'mg-%s-shell.log' % name)
                for p in (shot, log_path):
                    if os.path.isfile(p):
                        os.remove(p)
                rc, log = run_shell_frame(
                    shell, g['dir'], os.path.basename(g['script']),
                    SHELL_SECONDS, shot, log_path)
                check(rc == 0, 'shell exit 0 (got %s)' % rc)
                size = os.path.getsize(shot) if os.path.isfile(shot) else 0
                check(size > 1_000_000,
                      'frame BMP written (%d bytes)' % size)
                m = log_match(r'^vm\s+(\d+) steps total, (\d+) text lines',
                              log)
                check(m is not None and int(m.group(1)) > 0,
                      'vm kept stepping (%s)'
                      % ((m.group(0) if m else 'missing')))
                m = log_match(r'^unimpl cmds\s+(\S+)', log)
                check(m is not None and m.group(1) == 'none',
                      'unimpl cmds none (got %s)'
                      % (m.group(1) if m else 'missing'))
                if atlas:
                    m = log_match(r'^glyphs\s+(\d+) tofu, (\d+) from fallback',
                                  log)
                    check(m is not None and int(m.group(1)) == 0,
                          'no tofu glyphs (%s)'
                          % ((m.group(0) if m else 'missing')))
                m = log_match(r'^\[\s*[0-9.]+\] screenshot: .* (\d+)x(\d+) '
                              r'\((\d+) bytes\) ok', log)
                check(m is not None, 'screenshot log line present (%s)'
                      % ((m.group(0) if m else 'missing')))

                # 再来一次短跑，开 --text-prims：文本窗口按 PrimSetText(86) 绑定的
                # 图元位置画（原版做法），必须真的画出至少一个。
                shot2 = os.path.join(OUT_DIR, 'mg-%s-textprims.bmp' % name)
                log2_path = os.path.join(OUT_DIR,
                                         'mg-%s-textprims.log' % name)
                rc2, log2 = run_shell_frame(
                    shell, g['dir'], os.path.basename(g['script']), 4, shot2,
                    log2_path, extra=['--text-prims'])
                check(rc2 == 0, 'shell --text-prims exit 0 (got %s)' % rc2)
                m = log_match(r'^text prims\s+(\d+) drawn, (\d+) bound', log2)
                check(m is not None and int(m.group(1)) > 0,
                      'text prims drawn (%s)'
                      % ((m.group(0) if m else 'missing')))
        print('')

    # 6.5 存档请求：没给 --save-template 时必须拒绝写，且**不能**写进游戏目录
    if not args.no_shell:
        shell_sr = shell_path()
        if shell_sr is not None:
            for g in have:
                if g.get('known_blocker') or not g.get('dir'):
                    continue
                save_dir = os.path.join(g['dir'], 'save')
                before = (sorted(os.listdir(save_dir))
                          if os.path.isdir(save_dir) else [])
                shot = os.path.join(OUT_DIR, 'mg-%s-savereq.bmp' % g['name'])
                log_path = os.path.join(OUT_DIR, 'mg-%s-savereq.log' % g['name'])
                rc, log = run_shell_frame(
                    shell_sr, g['dir'], os.path.basename(g['script']), 1, shot,
                    log_path, extra=['--debug-save-request', '12'])
                check(rc == 0, 'save-request run exit 0 (got %s)' % rc)
                check('refusing to invent a snapshot' in log,
                      'save request without --save-template is refused')
                after = (sorted(os.listdir(save_dir))
                         if os.path.isdir(save_dir) else [])
                check(before == after,
                      'save request wrote nothing into the game save dir '
                      '(%d -> %d entries)' % (len(before), len(after)))
            # 6.9 存档预览：选中槽里记了背景图名时，列表要显示那张图
    if not args.no_shell:
        shell_pv = shell_path()
        if shell_pv is not None:
            for g in have:
                t = g.get('title') or {}
                if t.get('preview_slot', -1) < 0 or not g.get('dir'):
                    continue
                name = g['name']
                shot_pv = os.path.join(OUT_DIR, 'mg-%s-preview.bmp' % name)
                log_pv = os.path.join(OUT_DIR, 'mg-%s-preview.log' % name)
                for p4 in (shot_pv, log_pv):
                    if os.path.isfile(p4):
                        os.remove(p4)
                rc4, log4 = run_shell_frame(
                    shell_pv, g['dir'], os.path.basename(g['script']), 2,
                    shot_pv, log_pv,
                    extra=['--title-save-list',
                           '--preview-slot', str(t['preview_slot'])])
                check(rc4 == 0, 'save preview run exit 0 (got %s)' % rc4)
                m = log_match(r"preview graph '([^']+)' loaded", log4)
                check(m is not None,
                      'save preview graph loaded (%s)'
                      % (m.group(0) if m else 'missing'))
                img = read_bmp32(shot_pv) if os.path.isfile(shot_pv) else None
                check(img is not None, 'save preview frame readable')
                if img is not None:
                    rows_pv = img[2]
                    lit = 0
                    for yy in range(640, 1000, 8):
                        row = rows_pv[yy]
                        for xx in range(1200, 1840, 8):
                            v = row[xx * 4] + row[xx * 4 + 1] + row[xx * 4 + 2]
                            if v > 120:
                                lit += 1
                    check(lit > 200,
                          'preview area shows the saved background '
                          '(%d lit samples)' % lit)

                # 部分恢复视图：Enter 选中槽 → 整屏背景 + 台词 + [PARTIAL RESTORE]
                shot_lv = os.path.join(OUT_DIR, 'mg-%s-loadview.bmp' % name)
                log_lv = os.path.join(OUT_DIR, 'mg-%s-loadview.log' % name)
                for p5 in (shot_lv, log_lv):
                    if os.path.isfile(p5):
                        os.remove(p5)
                rc5, log5 = run_shell_frame(
                    shell_pv, g['dir'], os.path.basename(g['script']), 2,
                    shot_lv, log_lv,
                    extra=['--title-save-list',
                           '--load-slot', str(t['preview_slot'])])
                check(rc5 == 0, 'partial restore run exit 0 (got %s)' % rc5)
                check('load: partial restore slot=%d' % t['preview_slot'] in log5,
                      'partial restore logged (not a silent fake load)')
                img5 = read_bmp32(shot_lv) if os.path.isfile(shot_lv) else None
                check(img5 is not None, 'partial restore frame readable')
                if img5 is not None:
                    rows5 = img5[2]
                    lum5 = 0
                    cnt5 = 0
                    for yy in range(0, 1080, 24):
                        row5 = rows5[yy]
                        for xx in range(0, 1920, 24):
                            lum5 += (row5[xx * 4] + row5[xx * 4 + 1] +
                                     row5[xx * 4 + 2])
                            cnt5 += 1
                    mean5 = lum5 / (3.0 * cnt5)
                    check(mean5 > 80.0,
                          'restored background fills the screen (mean %.0f)'
                          % mean5)
                print('')

    # 7. 标题菜单：两部作品的 profile与菜单几何都要对
    if not args.no_shell:
        shell2 = shell_path()
        if shell2 is not None:
            stage_title_menu(shell2, have, check)
            stage_opening(shell2, have, check)

    if fails:
        print('verify_multigame: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_multigame: PASS (%d game(s))' % len(have))
    return 0


if __name__ == '__main__':
    sys.exit(main())