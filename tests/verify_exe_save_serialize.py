#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""存档槽序列化器的门禁：exe 静态分析冻结的写入顺序 + 真实存档的数据校验。

事实来源：`scripts/extract-exe-save-serialize.py` 从 `iroseka_HD.exe` 的
序列化器 0x43DE90..0x43E67B 里读出的**写入顺序、段来源、判据字节**，
基线 `ref/exe-save-serialize.json`。

三层校验：
1. 重新提取 → 与基线逐字节一致（exe 里那段指令流变了就报错）；
2. 基线的结构自检（31 段、5 段文件头、26 段槽尾、末两字节写出规则）；
3. **数据侧**：拿 6 份真实存档按这个模型走一遍，验证可证伪的推论
   （协程快照紧贴 tail 起点、128 槽字符串表正好在 +0x16608 结束、
   tail+0x18 = 02、文件末两字节 01 01）。缺存档/缺 exe 时 SKIP。

用法：python tests/verify_exe_save_serialize.py
"""
import io
import json
import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FVP = os.path.dirname(HERE)
PY = sys.executable
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-save-serialize.py')
REF = os.path.join(FVP, 'ref', 'exe-save-serialize.json')
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
SAVE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD', 'save')
DOC = os.path.join(FVP, 'docs', 'save-format.md')
SLOTS = ('s481', 's485', 's800', 's901', 's905', 's906')
CTX_COUNT = 32
CTX_STRIDE = 0xB28
CTX_BYTES = CTX_COUNT * CTX_STRIDE

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read_header(data):
    """7 字节时间戳 + 3 条 u16 长度字符串 → 返回 (thumb_off, strings)。"""
    off = 7
    strs = []
    for _ in range(3):
        ln = struct.unpack_from('<H', data, off)[0]
        off += 2
        strs.append(data[off:off + ln])
        off += ln
    return off, strs


def parse_string_table(blob, off, n=128):
    cur = off
    out = []
    for _ in range(n):
        if cur + 2 > len(blob):
            return None, None
        ln = struct.unpack_from('<H', blob, cur)[0]
        cur += 2
        if ln > 0x1000 or cur + ln > len(blob):
            return None, None
        s = blob[cur:cur + ln]
        if ln and s[-1] != 0:
            return None, None
        out.append(s)
        cur += ln
    return cur, out


def main():
    if not os.path.isfile(EXE):
        print('SKIP: 缺 exe %s' % EXE)
        return 0
    for p in (EXTRACT, REF, DOC):
        if not os.path.isfile(p):
            print('FAIL: 缺 %s' % p)
            return 1

    print('== 1. 重新提取并与基线比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=300)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    if not check(proc.returncode == 0,
                 'extract-exe-save-serialize.py --check 退出码 0'):
        return 1

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. 基线的结构自检 ==')
    ser = ref['serializer']
    check(ser['fn'] == '0x0043de90', '序列化器入口 0x43DE90')
    check(ser['end'] == '0x0043e67b', '序列化器出口 0x43E67B')
    check(2000 < ser['size'] < 2100,
          '序列化器长度 %d 字节（实测 2028）' % ser['size'])
    segs = ref['segments']
    check(len(segs) == 31, '段数 31（实际 %d）' % len(segs))
    preamble = [s for s in segs if s['stage'] == 'preamble']
    tail = [s for s in segs if s['stage'] == 'tail']
    check(len(preamble) == 5, '文件头 5 段：时间戳/3 字符串/缩略图')
    check(len(tail) == 26, '槽尾 26 段（实际 %d）' % len(tail))
    check([s['order'] for s in segs] == list(range(1, 32)), 'order 连续 1..31')
    labels = [s['label'] for s in tail]
    check(labels[0] == 'per-slot globals', '槽尾第一段 = 每槽全局变量')
    check(labels[1] == 'coroutine snapshots', '槽尾第二段 = 32 个协程快照')
    check('prim array' in labels, '槽尾含 prim 数组（0x34000 定长）')
    check('history ring' in labels,
          '槽尾含台词历史环（第 20 段 0x438120，见 ref/exe-history.json）')
    check(labels[-2:] == ['byte at 0x458', 'byte at 0x85 != 0'],
          '末两段 = 0x458 / 0x85!=0 两个字节')
    prim = [s for s in tail if s['label'] == 'prim array'][0]
    check(prim['size'] == 0x34000, 'prim 数组长度写死 0x34000（4096×0x34）')
    check(prim['src'] == 'engine+0x61dd64', 'prim 数组来源 engine+0x61DD64')
    ctx = tail[1]
    check(ctx['count'] == 32 and ctx['stride'] == 0xB28,
          '32 个协程快照 × 0xB28')
    check(ctx['call'] == '0x00445640', '协程快照子序列化器 0x445640')
    st = [s for s in tail if s['label'] == 'dynamic string table'][0]
    check(st['count'] == 0x80, '动态字符串表 128 槽')
    check(st['writer'] == '0x00434410', '字符串写法 0x434410（u16 长度含 NUL）')
    check(ref['helpers']['string_u16']['empty_sentinel'] == '0x00481364',
          '空串哨兵 0x481364（只写 u16 0）')
    check(len(ref['unknown']) >= 3, '基线保留未解条目 %d 条'
          % len(ref['unknown']))
    check(len(ref['invariants']) == 4, '4 条数据侧推论写进基线')

    print('== 3. 数据侧：6 份真实存档按模型走一遍 ==')
    if not os.path.isdir(SAVE):
        print('  SKIP: 缺存档目录 %s' % SAVE)
    else:
        counts = {'ctx_data': 0, 'table_ok': 0, 'markers': 0, 'len_ok': 0}
        for slot in SLOTS:
            path = os.path.join(SAVE, slot + '.bin')
            if not os.path.isfile(path):
                print('  SKIP: %s 不存在' % path)
                continue
            data = open(path, 'rb').read()
            thumb_off, strs = read_header(data)
            len_ok = all((len(s) == 0) or s[-1] != 0 for s in strs)
            if len_ok:
                counts['len_ok'] += 1
            thumb = 200 * 113 * 4
            if len(data) < thumb_off + thumb:
                check(False, '%s: 缩略图越界' % slot)
                continue
            blob = data[thumb_off + thumb:]
            # 32 个协程快照：0..7 号都应有数据（实测现象，8 号之后基本为零）
            live = [i for i in range(CTX_COUNT)
                    if any(blob[i * CTX_STRIDE:(i + 1) * CTX_STRIDE])]
            ctx_ok = len(live) >= 8 and live[:8] == list(range(8))
            if ctx_ok:
                counts['ctx_data'] += 1
            # 128 槽字符串表正好在 +0x16608 结束
            end, slots = parse_string_table(blob, CTX_BYTES + 8)
            table_ok = (end == CTX_BYTES + 8 + 0x80 * 2 and
                        sum(1 for s in slots if s) == 0)
            if table_ok:
                counts['table_ok'] += 1
            markers = blob[0x18] == 2 and blob[-2:] == b'\x01\x01'
            if markers:
                counts['markers'] += 1
            check(ctx_ok, '%s: 协程快照 0..7 有数据（活跃 %d 个）' % (slot, len(live)))
            check(table_ok, '%s: 128 槽字符串表全空且止于 +0x16608（实际 %#x）'
                  % (slot, end if end else 0))
            check(markers, '%s: tail+0x18 = %#04x，末两字节 %s'
                  % (slot, blob[0x18], blob[-2:].hex()))
            check(len_ok, '%s: 槽头字符串长度不含 NUL（0x43DFCD 的写法）' % slot)
        n = len(SLOTS)
        check(counts['ctx_data'] == n, '6 份都符合协程快照分布（%d/%d）'
              % (counts['ctx_data'], n))
        check(counts['table_ok'] == n, '6 份都符合 128 槽表结构（%d/%d）'
              % (counts['table_ok'], n))
        check(counts['markers'] == n, '6 份都符合首尾标记（%d/%d）'
              % (counts['markers'], n))
        check(counts['len_ok'] == n, '6 份都符合槽头字符串长度约定（%d/%d）'
              % (counts['len_ok'], n))

    print('== 4. 文档记录 ==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('0x43DE90' in doc, 'save-format 写了序列化器入口 0x43DE90')
    check('128 槽' in doc or '128槽' in doc, '文档写了 128 槽字符串表')
    check('协程快照' in doc, '文档写了协程快照')
    check('0x16608' in doc, '文档写了槽尾可前向走到的 0x16608 锚点')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 存档槽序列化顺序已从 exe 冻结（31 段：5 文件头 + 26 槽尾），'
          '并用 6 份真实存档验证了可证伪的推论（协程快照紧贴 tail 起点、'
          '128 槽字符串表止于 0x16608、首尾标记）；各子序列化器长度仍列为未解')
    return 0


if __name__ == '__main__':
    sys.exit(main())