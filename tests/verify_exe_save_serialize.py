#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""存档槽序列化器的门禁：exe 静态分析冻结的写入顺序 + 真实存档的数据校验。

事实来源：`scripts/extract-exe-save-serialize.py` 从 `iroseka_HD.exe` 的
序列化器 0x43DE90..0x43E67B 里读出的**写入顺序、段来源、判据字节**，
基线 `ref/exe-save-serialize.json`。

三层校验：
1. 重新提取 → 与基线逐字节一致（exe 里那段指令流变了就报错）；
2. 基线的结构自检（31 段、5 段文件头、26 段槽尾、末两字节写出规则、
   子读/写器的固定字节数）；
3. **数据侧**：拿 6 份真实存档按这个模型走一遍，验证可证伪的推论
   （`tail[0:0x3068]` = 1549 条每槽全局变量 → 32 个上下文 0x163A0 →
   线程下标 @0x19408 → 128 槽动态字符串表 @0x19410 → … → +0x600 处是
   台词历史环，环记录起点与独立扫描值逐份一致），并校验读档侧 20 步游标顺序。
   缺存档/缺 exe 时 SKIP。

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

GLOBALS_PER_SLOT = 1549      # 槽尾写的 G1 条（save.bin 写同一数组的后 1471 条）
GLOBALS_BYTES = GLOBALS_PER_SLOT * 8          # 0x3068
CTX_COUNT = 32
CTX_ITEM = 0xB1D             # 单个协程上下文写进文件的固定字节数
CTX_STRIDE_INMEM = 0xB28     # 内存里对象步长（多出的 11 字节不写盘）
CTX_BYTES = CTX_COUNT * CTX_ITEM              # 0x163A0
POOL_BASE = 0x163A0          # 线程下标 u32 的基址（全局变量 0 条时）
THREAD_NONE = 0x20
SLOT_COUNT = 0x80
WALK_GAP = 0x600             # 调色板与历史环之间尚未逆清的定长块
# 独立扫描出来的“历史环第一条记录”偏移（旧报告里的 ring 位置就是这么来的）
RING_RECORD_OFF = {
    's481': 0x20622, 's485': 0x328ce, 's800': 0x328c5,
    's901': 0x1AEAD, 's905': 0x20633, 's906': 0x1AEAD,
}
RING_COUNT = {'s481': 1, 's485': 1, 's800': 1, 's901': 1, 's905': 11, 's906': 1}
STR_COUNT = {'s481': 1, 's485': 1, 's800': 0, 's901': 0, 's905': 2, 's906': 0}

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


def parse_string_table(blob, off, n):
    """逐槽 u16 长度（含 NUL）+ 字节，空槽 2 字节 0。返回 (end, slots)。"""
    cur = off
    out = []
    for _ in range(n):
        if cur + 2 > len(blob):
            return None, None
        ln = struct.unpack_from('<H', blob, cur)[0]
        cur += 2
        if ln > 0x200 or cur + ln > len(blob):
            return None, None
        s = blob[cur:cur + ln]
        if ln and s[-1] != 0:
            return None, None
        out.append(s)
        cur += ln
    return cur, out


def walk_to_ring(blob):
    """按修正后的模型从 tail 开头走到台词历史环，返回 (环偏移, 诊断 dict)。"""
    pos = GLOBALS_BYTES + CTX_BYTES
    thread = struct.unpack_from('<I', blob, pos)[0]
    str_cnt = struct.unpack_from('<I', blob, pos + 4)[0]
    pos += 8
    end, slots = parse_string_table(blob, pos, SLOT_COUNT)
    if end is None:
        return None, {'error': '字符串表解析失败'}
    nonempty = sum(1 for s in slots if s)
    pos = end + 0x100 + 0x100
    u = struct.unpack_from('<I', blob, pos)[0]
    pos += 4
    item_counts = []
    for _ in range(SLOT_COUNT):
        c = struct.unpack_from('<I', blob, pos)[0]
        item_counts.append(c)
        pos += 12 + 12 * c
        if pos > len(blob):
            return None, {'error': '128 项表越界'}
    pos += 0x200 + 0x200 + 1 + 16 * 9 + 0x400
    diag = {'thread': thread, 'str_count': str_cnt, 'nonempty': nonempty,
            'u32_0x6a4dec': u, 'items_nonempty': sum(1 for c in item_counts if c),
            'walk_end': pos, 'ring': pos + WALK_GAP}
    return pos + WALK_GAP, diag


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
          '32 个协程快照、内存步长 0xB28')
    check(ctx['call'] == '0x00445640', '协程快照子序列化器 0x445640')
    st = [s for s in tail if s['label'] == 'dynamic string table'][0]
    check(st['count'] == 0x80, '动态字符串表 128 槽')
    check(st['writer'] == '0x00434410', '字符串写法 0x434410（u16 长度含 NUL）')
    check(ref['helpers']['string_u16']['empty_sentinel'] == '0x00481364',
          '空串哨兵 0x481364（只写 u16 0）')
    check(len(ref['unknown']) >= 2, '基线保留未解条目 %d 条'
          % len(ref['unknown']))
    check(len(ref['invariants']) == 6, '6 条数据侧推论写进基线（实际 %d）'
          % len(ref['invariants']))

    print('== 2b. 子读/写器的长度规则 ==')
    subs = {s['id']: s for s in ref['sub_readers']}
    check(set(subs) == {'context', 'item128', 'savebin_globals_split'},
          '冻结 3 个子读/写器（实际 %s）' % sorted(subs))
    check(subs['context']['bytes_fixed'] == CTX_ITEM,
          '协程上下文固定 %#x 字节（32 条 = %#x）'
          % (CTX_ITEM, CTX_BYTES))
    check(subs['context']['fn'] == '0x00445540',
          '上下文反序列化器 0x445540')
    check('12 + 12×count' in subs['item128']['bytes_rule'],
          '128 项表每项 12 + 12×count 字节')
    check(subs['item128']['fn'] == '0x00446210', '128 项表读取器 0x446210')
    check(subs['savebin_globals_split']['fn'] == '0x00439743',
          'save.bin 全局量区间 0x439743')
    samples = ref['samples']
    check(samples['g1'] == GLOBALS_PER_SLOT,
          '基线样本 G1 = %d 条' % samples['g1'])
    check(samples['g1'] + samples['g2'] == samples['max_script_global_index'] + 1,
          'G1+G2 = 最大脚本全局下标 + 1（%d = %d + 1）'
          % (samples['g1'] + samples['g2'], samples['max_script_global_index']))
    check(int(samples['globals_bytes'], 16) == GLOBALS_BYTES,
          'G1×8 = %#x' % GLOBALS_BYTES)
    check(int(samples['context_bytes'], 16) == CTX_BYTES,
          '32×0xB1D = %#x' % CTX_BYTES)
    check(int(samples['thread_index_u32_offset'], 16) ==
          GLOBALS_BYTES + CTX_BYTES,
          '线程下标偏移 = 0x3068 + 0x163A0 = %#x'
          % (GLOBALS_BYTES + CTX_BYTES))

    print('== 2c. 读档侧（反序列化）顺序 ==')
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
        counts = {'globals': 0, 'strings': 0, 'walk': 0, 'markers': 0,
                  'len_ok': 0}
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

            # 3a. 前 G1 条全局变量：8 字节 Variant，类型字节 0..6
            types = [blob[i * 8] for i in range(GLOBALS_PER_SLOT)]
            globals_ok = (all(t <= 6 for t in types) and
                          types.count(2) > 0 and blob[0x18] == 2)
            if globals_ok:
                counts['globals'] += 1
            check(globals_ok,
                  '%s: tail[0:%#x] = %d 条全局变量（type2 %d 条、type5 %d 条、'
                  '第 4 条 type=%d）'
                  % (slot, GLOBALS_BYTES, GLOBALS_PER_SLOT, types.count(2),
                     types.count(5), blob[0x18]))

            # 3b. 动态字符串表：线程下标 0x20 + 条数 = type5 条数
            tb = GLOBALS_BYTES + CTX_BYTES
            thread = struct.unpack_from('<I', blob, tb)[0]
            str_cnt = struct.unpack_from('<I', blob, tb + 4)[0]
            end, slots = parse_string_table(blob, tb + 8, SLOT_COUNT)
            strings_ok = (thread == THREAD_NONE and end is not None and
                          str_cnt == STR_COUNT[slot] and
                          sum(1 for s in slots if s) == str_cnt and
                          str_cnt == types.count(5))
            if strings_ok:
                counts['strings'] += 1
            check(strings_ok,
                  '%s: 线程下标 @%#x = %#x，非空字符串 %d 条（type5 也是 %d 条）'
                  % (slot, tb, thread, str_cnt, types.count(5)))

            # 3c. 前向走查到历史环：+0x600 后落点 == 独立扫描的记录起点 - 8
            ring, diag = walk_to_ring(blob)
            want_rec = RING_RECORD_OFF[slot]
            rc = struct.unpack_from('<I', blob, ring)[0] if ring else -1
            rp = struct.unpack_from('<I', blob, ring + 4)[0] if ring else -1
            walk_ok = (ring is not None and ring + 8 == want_rec and
                       rc == RING_COUNT[slot] and 0 <= rp < max(rc, 1))
            if walk_ok:
                counts['walk'] += 1
            check(walk_ok,
                  '%s: 走查终点 %#x + %#x → 历史环 count=%d pos=%d，'
                  '记录起点 %#x（独立扫描值 %#x）'
                  % (slot, diag.get('walk_end', 0), WALK_GAP, rc, rp,
                     (ring or 0) + 8, want_rec))

            markers = blob[-2:] == b'\x01\x01'
            if markers:
                counts['markers'] += 1
            check(markers, '%s: 末两字节 %s' % (slot, blob[-2:].hex()))
            check(len_ok, '%s: 槽头字符串长度不含 NUL（0x43DFCD 的写法）' % slot)
        n = len(SLOTS)
        check(counts['globals'] == n,
              '6 份都是"槽尾前 1549 条全局变量"模型（%d/%d）'
              % (counts['globals'], n))
        check(counts['strings'] == n,
              '6 份的动态字符串表都在 globals+0x163A0（%d/%d）'
              % (counts['strings'], n))
        check(counts['walk'] == n,
              '6 份都能从 tail 开头走到历史环（%d/%d）'
              % (counts['walk'], n))
        check(counts['markers'] == n, '6 份都符合末两字节标记（%d/%d）'
              % (counts['markers'], n))
        check(counts['len_ok'] == n, '6 份都符合槽头字符串长度约定（%d/%d）'
              % (counts['len_ok'], n))

    print('== 4. 文档记录 ==')
    doc = io.open(DOC, encoding='utf-8').read()
    check('0x43DE90' in doc, 'save-format 写了序列化器入口 0x43DE90')
    check('128 槽' in doc or '128槽' in doc, '文档写了 128 槽字符串表')
    check('协程快照' in doc, '文档写了协程快照')
    check('1549' in doc, '文档写了每槽全局变量 1549 条')
    check('0x3068' in doc, '文档写了每槽全局变量 0x3068 字节')
    check('0x163A0' in doc or '0x163a0' in doc, '文档写了 32 上下文 = 0x163A0')
    check('0x600' in doc, '文档写了调色板与历史环之间那段 0x600 未解块')
    check('savetail' in doc or '扫描定位' in doc, '文档写了字符串表位置靠扫描定位')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 存档槽写/读顺序都已冻结（写 31 段；读 20 步，游标从 7 开始）；'
          '数据侧 6 份真实存档验证了"tail 开头 = 1549 条全局变量（0x3068）→ '
          '32 上下文（0x163A0）→ 线程下标 @0x19408 → 128 槽字符串表 @0x19410 '
          '→ … → +0x600 处 = 台词历史环，记录起点与独立扫描逐份一致"')
    return 0


if __name__ == '__main__':
    sys.exit(main())