#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""文本必须来自 HCB 里的字符串（A 类逆向核对）。

为什么需要它：`FVP_FRAME_SLOTS=1` 的修正帧模型把正文窗口放到了脚本要求的位置，但它
的文本输出**每行多一个「」**。本轮用 HCB 的字符串数据做判据：

  * HCB 里 `……。` 出现 1345 次（cp932 `81 63 81 63 81 42`），
    而 `……。」`（追加 `81 76`）**一次都没有**；
  * 冻结基线 `ref/ref-900k.txt` 的每行（去掉 `[层] ` 前缀和行首的全角缩进）都能在
    HCB 字节流里找到；`FVP_FRAME_SLOTS=1` 的输出则**找不到**（多出来的「」是编的）。

所以：**冻结基线是对的，修正帧模型不能直接采用/重新冻结**——它虽然修好了窗口位置，
却让文本多读了一个字符，得先找到那个"多读一位"的原因。

门禁断言：
  1. 冻结基线的文本行 100% 能在 HCB 里找到（0 条编造）；
  2. `FVP_FRAME_SLOTS=1` 的文本行里有**至少一条**在 HCB 里找不到（说明它多读了）；
  3. 直接断言 `……。` 在 HCB 里存在、`……。」` 不存在（判据本身有出处）。

缺 `bin/fvm.exe` 或游戏数据时 SKIP 并返回 0。

用法：python tests/verify_text_from_hcb.py
"""
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
FVM = os.path.join(FVP, 'bin', 'fvm.exe')
REF = os.path.join(FVP, 'ref', 'ref-900k.txt')
OUT = os.path.join(FVP, 'out', 'c')
HCB = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                   '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD', 'iroseka.hcb')
STEPS = 900000
PREFIX = re.compile(r'^\[[0-9]+\]\s?')

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def norm(line):
    """去掉 `[层] ` 前缀与行首全角缩进，得到应该来自 HCB 的那段文本。"""
    s = PREFIX.sub('', line.rstrip('\r\n'))
    return s.lstrip('\u3000')


def in_hcb(data, text):
    if not text:
        return True
    try:
        pat = text.encode('cp932')
    except UnicodeEncodeError:
        return False
    return data.find(pat) >= 0


def lines_of(path):
    with io.open(path, encoding='utf-8', errors='replace') as fp:
        return [ln for ln in fp if ln.strip()]


def run_fvm(env_extra=None):
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    path = os.path.join(OUT, 'text-hcb-check.txt')
    if os.path.isfile(path):
        os.remove(path)
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    rc = subprocess.run([FVM, HCB, '--steps', str(STEPS), '--text-out', path,
                         '--quiet'], timeout=600, env=env).returncode
    if rc != 0 or not os.path.isfile(path):
        return None
    return lines_of(path)


def main():
    if not os.path.isfile(FVM) or not os.path.isfile(REF):
        print('SKIP: 缺 bin/fvm.exe 或 ref/ref-900k.txt')
        return 0
    if not os.path.isfile(HCB):
        print('SKIP: 缺游戏数据 %s' % HCB)
        return 0
    data = open(HCB, 'rb').read()

    print('== 1. 判据本身有出处 ==')
    check(in_hcb(data, '……。'), 'HCB 里有 `……。`')
    check(not in_hcb(data, '……。」'),
          'HCB 里**没有** `……。」`（「」不是原版文本的一部分）')

    print('== 2. 冻结基线的每一行都能在 HCB 找到 ==')
    lines = lines_of(REF)
    bad = [ln for ln in lines if not in_hcb(data, norm(ln))]
    check(lines and not bad,
          '冻结基线 %d 行，编造 %d 行（示例 %r）'
          % (len(lines), len(bad), bad[:3]))

    print('== 3. FVP_FRAME_SLOTS=1 的输出会多读字符 ==')
    alt = run_fvm({'FVP_FRAME_SLOTS': '1'})
    check(alt is not None, 'fvm（FVP_FRAME_SLOTS=1）跑出文本')
    if alt:
        bad_alt = [ln for ln in alt if not in_hcb(data, norm(ln))]
        check(len(alt) > 0, '修正模型输出 %d 行' % len(alt))
        check(len(bad_alt) > 0,
              '修正模型有 %d 行在 HCB 里找不到（例如 %r）'
              % (len(bad_alt), bad_alt[:2]))

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 冻结文本基线的每一行都来自 HCB；修正帧模型的输出多读字符，'
          '所以不能直接重新冻结')
    return 0


if __name__ == '__main__':
    sys.exit(main())