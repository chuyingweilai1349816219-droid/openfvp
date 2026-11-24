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
         script=os.path.join(DOWNLOADS, '五彩斑斓的世界HD', 'iroseka.hcb'),
         expect_lines=195),
    dict(name='hoshimemo',  # 《星空的记忆 HD》
         script=os.path.join(DOWNLOADS, 'hoshimemo_installed',
                             'Hoshimemo_HD.hcb'),
         expect_lines=None, min_lines=50),
    dict(name='sakura',  # 《樱花，萌放。（さくら、もゆ。）》
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--steps', type=int, default=900000)
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

    if fails:
        print('verify_multigame: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_multigame: PASS (%d game(s))' % len(have))
    return 0


if __name__ == '__main__':
    sys.exit(main())