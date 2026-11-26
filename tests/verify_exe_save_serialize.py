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
   （`tail[0:0x2DF8]` = 1471 条每槽全局变量、128 槽字符串表在 0x19300、
   字符串池标签 = 0x16610 + 全局变量字节数 = 0x19408、末两字节 01 01），
   并校验读档侧 20 步游标顺序。缺存档/缺 exe 时 SKIP。

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
GLOBALS_PER_SLOT = 1471          # 6 份真实存档里每槽全局变量的条数
POOL_BASE = 0x16610              # 全局变量 0 条时字符串池标签的偏移

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

    print('== 2b. 读档侧（反序列化）顺序 ==')
    rd = ref['reader']
    check(len(rd) == 20, '读档侧 %d 步（期望 20）' % len(rd))
    va = [r['va'] for r in rd]
    check(va[0] == '0x0043efcc' and rd[0]['frags'][0] == '8b06',
          '读档从偏移 7 开始（0x43EFCC：mov (%esi),%eax; movl $7,0x8(%esi)）')
    check(any('c8666a00' in r['bytes'] for r in rd),
          '每槽全局变量的条数来自 engine+0x6A66C8（0x43F0D4）')
    check(any('d00f0000' in r['bytes'] for r in rd),
          '缩略图按 w*h*4 推进游标（0x43F0C7）')
    check(any('8dbeec496a00' in r['bytes'] for r in rd),
          '128 槽字符串表逐条读（0x43F152 调 0x433F60）')
    check(any('8dbef04d6a00' in r['bytes'] for r in rd),
          '128 项表在 0x43F1CA（逐项读，不是整块 memcpy）')
    check(any('c0446a00' in r['bytes'] for r in rd),
          '调色板 → engine+0x6A44C0（0x43F24E）')
    check(rd[-1]['va'] == '0x0043f282' and
          rd[-1]['frags'] == ['8b8ec8496a00', '81ffb0040000'],
          '读档最后回填历史环（count/pos + 100 条，0x43F262/0x43F282）')

    print('== 3. 数据侧：6 份真实存档按模型走一遍 ==')
    if not os.path.isdir(SAVE):
        print('  SKIP: 缺存档目录 %s' % SAVE)
    else:
        counts = {'globals': 0, 'e_empty': 0, 'markers': 0, 'len_ok': 0,
                  'pool': 0}
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
            # 每槽全局变量：1471 条 × 8 字节，type 只出现 0/1/2/4/5/6
            gbytes = GLOBALS_PER_SLOT * 8
            types = [blob[i * 8] for i in range(GLOBALS_PER_SLOT)]
            globals_ok = (all(t <= 6 for t in types) and
                          types.count(2) > 0 and blob[0x18] == 2)
            if globals_ok:
                counts['globals'] += 1
            check(globals_ok, '%s: tail[0:%#x] = %d 条全局变量（type=2 的 %d 条，'
                  '第 4 条 type=%d）'
                  % (slot, gbytes, GLOBALS_PER_SLOT, types.count(2),
                     blob[0x18]))
            # 128 槽字符串表：0x2DF8 + 0x16500 + 8 + 0x100（样本全空）
            e_start = gbytes + CTX_BYTES + 8
            e_end, e_slots = parse_string_table(blob, e_start)
            e_ok = (e_end == e_start + 0x100 and
                    sum(1 for t in e_slots if t) == 0)
            if e_ok:
                counts['e_empty'] += 1
            check(e_ok, '%s: 128 槽字符串表 @%#x..%#x 全空'
                  % (slot, e_start, e_end if e_end else 0))
            # 字符串池标签：0x16610 + 1471×8 = 0x19408
            pool_off = POOL_BASE + gbytes
            tag = struct.unpack_from('<I', blob, pool_off)[0]
            cnt = struct.unpack_from('<I', blob, pool_off + 4)[0]
            pool_ok = tag == 0x20 and cnt <= 8
            if pool_ok:
                counts['pool'] += 1
            check(pool_ok, '%s: 字符串池标签 @%#x（0x16610 + %d）=%#x，条数 %d'
                  % (slot, pool_off, gbytes, tag, cnt))
            markers = blob[-2:] == b'\x01\x01'
            if markers:
                counts['markers'] += 1
            check(markers, '%s: 末两字节 %s' % (slot, blob[-2:].hex()))
            check(len_ok, '%s: 槽头字符串长度不含 NUL（0x43DFCD 的写法）' % slot)
        n = len(SLOTS)
        check(counts['globals'] == n, '6 份都符合"1471 条全局变量"模型（%d/%d）'
              % (counts['globals'], n))
        check(counts['e_empty'] == n, '6 份的 128 槽字符串表都在 globals 之后'
              '（%d/%d）' % (counts['e_empty'], n))
        check(counts['markers'] == n, '6 份都符合末两字节标记（%d/%d）'
              % (counts['markers'], n))
        check(counts['len_ok'] == n, '6 份都符合槽头字符串长度约定（%d/%d）'
              % (counts['len_ok'], n))
        check(counts['pool'] == n, '6 份都在 0x16610+1471×8 处找到字符串池标签'
              '（%d/%d）' % (counts['pool'], n))

    print('== 4. 文档记录 ==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('0x43DE90' in doc, 'save-format 写了序列化器入口 0x43DE90')
    check('128 槽' in doc or '128槽' in doc, '文档写了 128 槽字符串表')
    check('协程快照' in doc, '文档写了协程快照')
    check('0x16610' in doc, '文档写了字符串池基址 0x16610')
    check('1471' in doc, '文档写了每槽全局变量 1471 条')
    check('savetail' in doc or '扫描定位' in doc, '文档写了池位置靠扫描定位')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 存档槽写/读顺序都已冻结（写 31 段；读 20 步，游标从 7 开始）；'
          '数据侧 6 份真实存档验证了"tail 开头 = 1471 条全局变量（0x2DF8）→ '
          '128 槽字符串表（0x19300）→ 字符串池标签（0x19408 = 0x16610 + 1471×8）"；'
          '各子序列化器的绝对偏移仍列为未解')
    return 0


if __name__ == '__main__':
    sys.exit(main())