#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多游戏兼容性门禁：两部 FVP 作品都要能跑、可重复、文本编码正确。

为什么要有这个：openbgi 之所以是"引擎项目"而不是"一个游戏的移植"，靠的是
一部一部的兼容性矩阵。openfvp 之前只在 iroseka 上做逐字节等价，第二部
《星空的记忆 HD》(Hoshimemo_HD.hcb) 只是"手工跑过一次"。这个脚本把它变成
可重复的门禁。

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
]


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


def run_once(exe, script, steps, out_path):
    cmd = [exe, script, '--steps', str(steps), '--click', '600', '--quiet',
           '--text-out', out_path]
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = p.stdout.decode('utf-8', 'replace')
    return p.returncode, text


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
        rc_a, out_a = run_once(exe, g['script'], args.steps, a_path)
        rc_b, out_b = run_once(exe, g['script'], args.steps, b_path)
        check(rc_a == 0, 'run A exit 0 (got %d)' % rc_a)
        check(rc_b == 0, 'run B exit 0 (got %d)' % rc_b)
        ok_files = os.path.isfile(a_path) and os.path.isfile(b_path)
        check(ok_files, 'both text dumps written')
        if ok_files:
            same = sha256(a_path) == sha256(b_path)
            check(same, 'two runs are byte-identical (deterministic)')
            text = open(a_path, encoding='utf-8', errors='replace').read()
            lines, junk, ratio = text_stats(text)
            # 3. 有产出（行数判据只对 900k 步成立：步数变了行数自然变）
            if args.steps == BASELINE_STEPS:
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
            check(ratio > 0.5,
                  'japanese chars %.1f%% of non-space text' % (ratio * 100.0))
            print('     (%d lines, %d bytes, jp %.1f%%)'
                  % (lines, os.path.getsize(a_path), ratio * 100.0))
        print('')

    if fails:
        print('verify_multigame: %d check(s) FAILED' % len(fails))
        return 1
    print('verify_multigame: PASS (%d game(s))' % len(have))
    return 0


if __name__ == '__main__':
    sys.exit(main())