#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""帧槽模型门禁（FVP_FRAME_SLOTS）。

`src/vm.c` 的帧槽模型有两条路：
  * 默认：旧行为（`initstack` 第一个字节当局部变量个数），冻结文本基线用的就是它；
  * `FVP_FRAME_SLOTS=1`：修正版——参数槽 = 绝对槽 255-A..254，局部变量块在参数块
    下面（证据见 ref/script-window-geometry.json 的 frame_model 段：
    `0x23804`(2|0) 参数 253/254、`0x3F389`(8|0) 参数 251、`0x39F27`(5|0) 参数 250..254）。

本门禁断言：

  1. 默认模式下 900k 步的文本输出 == 冻结基线 `ref/ref-900k.txt`（sha256 一致）；
  2. 默认模式下正文窗口图元 551 是 (1,1)（已知差异，记在基线 known_gap 里）；
  3. `FVP_FRAME_SLOTS=1` 时正文窗口图元 551 = **(431, 880)**，与脚本设计坐标
     (230,522) × (1.875, 1.6875) 一致；名字/章节窗口仍是 (375,852)/(165,590)；
  4. `FVP_FRAME_SLOTS=1` 的 900k 文本输出与冻结基线**不同**（说明基线是旧模型生成的，
     重新冻结要单独决策，不能顺手改）。

缺 `bin/fvm.exe` / `bin/fvp-headless.exe` / 游戏数据时 SKIP 并返回 0。

用法：python tests/verify_frame_slots.py
"""
import hashlib
import io
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
FVM = os.path.join(FVP, 'bin', 'fvm.exe')
HEADLESS = os.path.join(FVP, 'bin', 'fvp-headless.exe')
REF = os.path.join(FVP, 'ref', 'ref-900k.txt')
GEO = os.path.join(FVP, 'ref', 'script-window-geometry.json')
OUT = os.path.join(FVP, 'out', 'c')
HCB = os.path.join('C:\\Users\\Mechrevo\\Downloads',
                   '\u4e94\u5f69\u6591\u6593\u7684\u4e16\u754cHD', 'iroseka.hcb')
STEPS = 900000
BODY_STEPS = 3000000

FROZEN_SHA = '7f9837ffaf3b7b5759581c625698c9554e83f2cdbdeadf635dca6fc9c01b3ea2'

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def run(argv, env_extra=None, timeout=900):
    env = dict(os.environ)
    if env_extra:
        env.update(env_extra)
    p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, env=env)
    return p.returncode, p.stdout.decode('utf-8', 'replace')


def text_sha(env_extra=None):
    if not os.path.isdir(OUT):
        os.makedirs(OUT)
    path = os.path.join(OUT, 'frame-slots-text.txt')
    if os.path.isfile(path):
        os.remove(path)
    rc, out = run([FVM, HCB, '--steps', str(STEPS), '--text-out', path,
                   '--quiet'], env_extra)
    if rc != 0 or not os.path.isfile(path):
        return None, 0, out
    data = open(path, 'rb').read()
    return hashlib.sha256(data).hexdigest(), len(data), out


def body_prim(env_extra=None):
    """Return the (x, y) the engine gives PrimSetText(551, layer 0)."""
    env = {'FVP_DEBUG_TEXT': '1'}
    if env_extra:
        env.update(env_extra)
    rc, out = run([HEADLESS, HCB, '--steps', str(BODY_STEPS)], env)
    for ln in out.splitlines():
        if 'PrimSetText prim=551 ' in ln:
            x = y = None
            for tok in ln.split():
                if tok.startswith('x='):
                    x = int(tok[2:])
                elif tok.startswith('y='):
                    y = int(tok[2:])
            return (x, y), out
    return None, out


def main():
    for p in (FVM, HEADLESS, REF):
        if not os.path.isfile(p):
            print('SKIP: 缺 %s' % p)
            return 0
    if not os.path.isfile(HCB):
        print('SKIP: 缺游戏数据 %s' % HCB)
        return 0

    print('== 1. 默认模式：文本输出 == 冻结基线 ==')
    sha, n, out = text_sha()
    check(sha == FROZEN_SHA,
          '默认模式 sha256 == 冻结基线（实际 %s, %d B）' % (sha, n))

    print('== 2. 默认模式：正文窗口图元 551（已知差异）==')
    xy, out = body_prim()
    check(xy == (1, 1), '默认模式 PrimSetText(551) = (1, 1)（实际 %r）' % (xy,))

    print('== 3. FVP_FRAME_SLOTS=1：正文窗口 = 脚本设计坐标 × 换算 ==')
    xy2, out2 = body_prim({'FVP_FRAME_SLOTS': '1'})
    check(xy2 == (431, 880),
          'FVP_FRAME_SLOTS=1 时 PrimSetText(551) = (431, 880)（实际 %r）' % (xy2,))
    for want in ('PrimSetText prim=552 tw=8 x=375 y=852',
                 'PrimSetText prim=553 tw=9 x=165 y=590',
                 'PrimSetText prim=558 tw=10 x=0 y=0'):
        check(want in out2, '同在：%s' % want)

    print('== 4. FVP_FRAME_SLOTS=1 的文本输出与冻结基线不同 ==')
    sha2, n2, _ = text_sha({'FVP_FRAME_SLOTS': '1'})
    check(sha2 is not None and sha2 != FROZEN_SHA,
          '修正模型的文本输出与基线不同（%s, %d B vs 冻结 6496 B）'
          % ((sha2 or '?')[:16], n2))
    print('     修正模型的文本输出：%d B, sha256 %s' % (n2, sha2))
    if os.path.isfile(GEO):
        geo = io.open(GEO, encoding='utf-8').read()
        check('known_gap' in geo and '431' in geo,
              '基线里记录了 known_gap 与期望画布 (431,880)')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 默认路径保持冻结基线；FVP_FRAME_SLOTS=1 时正文窗口落到脚本要求的 (431,880)')
    return 0


if __name__ == '__main__':
    sys.exit(main())