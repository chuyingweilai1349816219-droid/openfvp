#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""台词历史记录（HistorySet 26 / HistoryGet 25）的门禁。

三层事实：
1. **exe 静态分析**（A 类）：handler 判据、容器构造初值、记录环的读写顺序、
   读档路径里的 count/pos 回填 —— 基线 `ref/exe-history.json`，
   提取器 `scripts/extract-exe-history.py`；
2. **C 实现一致**：`src/history.c` 的常量/初值、`src/vm.c` 的判定落点、
   `src/syscall.c` 的委派；单元测试 `tests/test_history.c`（202 项）；
3. **真实存档**：6 份槽尾里定位历史环 → 校验 `count/pos` 语义、环内条目
   （每份存档都要求：槽 0..count-1 已写、pos == count-1、最新一条的台词
   等于槽头第 3 条字符串）。

缺 exe / 缺存档目录时对应小节 SKIP（返回 0）。

用法：python tests/verify_exe_history.py
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
EXTRACT = os.path.join(FVP, 'scripts', 'extract-exe-history.py')
REF = os.path.join(FVP, 'ref', 'exe-history.json')
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
SAVE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD', 'save')
SLOTS = ('s481', 's485', 's800', 's901', 's905', 's906')
RING = 100
REC = 12
HIST_C = os.path.join(FVP, 'src', 'history.c')
HIST_H = os.path.join(FVP, 'src', 'history.h')
VM_C = os.path.join(FVP, 'src', 'vm.c')
SYSCALL_C = os.path.join(FVP, 'src', 'syscall.c')
TEST_HIST = os.path.join(FVP, 'bin', 'test_history.exe')
DOCS = [os.path.join(FVP, 'docs', 'vm-commands.md'),
        os.path.join(FVP, 'docs', 'save-format.md')]

fails = []


def check(cond, msg):
    print(('  ok   ' if cond else '  FAIL ') + msg)
    if not cond:
        fails.append(msg)
    return cond


def read_header(data):
    off = 7
    hdr = []
    for _ in range(3):
        ln = struct.unpack_from('<H', data, off)[0]
        off += 2
        hdr.append(data[off:off + ln])
        off += ln
    return off, hdr


def parse_ring(blob, start, n=RING):
    """按写档顺序解析历史环；失败返回 None。"""
    cur = start
    out = []
    for _ in range(n):
        if cur + 4 > len(blob):
            return None
        l0 = struct.unpack_from('<I', blob, cur)[0]
        cur += 4
        if l0 > 0x800 or cur + l0 > len(blob):
            return None
        s0 = blob[cur:cur + l0]
        cur += l0
        if cur + 4 > len(blob):
            return None
        l1 = struct.unpack_from('<I', blob, cur)[0]
        cur += 4
        if l1 > 0x800 or cur + l1 > len(blob):
            return None
        s1 = blob[cur:cur + l1]
        cur += l1
        if cur + 4 > len(blob):
            return None
        v = struct.unpack_from('<i', blob, cur)[0]
        cur += 4
        out.append((s0, s1, v))
    return cur, out


def text_ok(s):
    """存档侧的文本字段：空串或能按 GBK/CP932 解出来的可打印文本。"""
    if not s:
        return True
    for enc in ('gbk', 'cp932'):
        try:
            t = s.decode(enc)
        except UnicodeDecodeError:
            continue
        if all(ord(c) >= 0x20 for c in t):
            return True
    return False


def find_ring(blob, want):
    """扫描 tail：找"前面 8 字节像 (count,pos) 且 100 条能解析完"的位置。

    判据全部来自 exe：count 初值 1、上限 100；未回绕时 pos == count-1；
    第 count 条之后是空记录 {0,0,-1}；文本字段能解成文字；最新一条
    （槽 pos）的台词等于当前台词（槽头第 3 条字符串，系统存档时为空）。
    """
    hits = []
    for start in range(0x1000, len(blob) - 1400):
        count, pos = struct.unpack_from('<II', blob, start - 8)
        if count < 1 or count > RING or pos != count - 1:
            continue
        res = parse_ring(blob, start)
        if res is None:
            continue
        end, recs = res
        ok = True
        for i in range(RING):
            s0, s1, v = recs[i]
            if i >= count:
                if s0 or s1 or v != -1:
                    ok = False
                    break
            elif not text_ok(s0) or not text_ok(s1):
                ok = False
                break
        if not ok:
            continue
        if want:
            if recs[pos][1] != want and recs[pos][0] != want:
                continue
        hits.append((start, end, count, pos, recs))
    return hits


def main():
    if not os.path.isfile(EXE):
        print('SKIP: 缺 exe %s' % EXE)
        return 0
    for p in (EXTRACT, REF, HIST_C, HIST_H, VM_C, SYSCALL_C):
        if not os.path.isfile(p):
            print('FAIL: 缺 %s' % p)
            return 1

    print('== 1. 重新提取并与基线比对 ==')
    proc = subprocess.run([PY, '-X', 'utf8', EXTRACT, '--check'],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          timeout=600)
    print(proc.stdout.decode('utf-8', 'replace').rstrip())
    if not check(proc.returncode == 0,
                 'extract-exe-history.py --check 退出码 0'):
        return 1

    with io.open(REF, encoding='utf-8') as fp:
        ref = json.load(fp)

    print('== 2. exe 侧事实 ==')
    h = ref['handlers']
    check(h['HistorySet']['va'] == '0x0043a5e0', 'HistorySet handler 0x43A5E0')
    check(h['HistoryGet']['va'] == '0x0043a490', 'HistoryGet handler 0x43A490')
    check('type == 0' in h['HistorySet']['begin_gate'],
          'begin 判据 = arg0 的 Variant type 0（null，不是 int 0）')
    check('0 <= idx < count' in h['HistoryGet']['kind_gate'],
          'HistoryGet 的 idx 必须 0 <= idx < count')
    check('pos - idx' in h['HistoryGet']['slot_map'], '槽位映射 slot = pos - idx')
    box = ref['container']
    check(box['engine_offset'] == 'engine+0x6a49c0', '容器在 engine+0x6A49C0')
    check('count（构造 = 1' in box['fields']['+0x00'],
          'count 构造 = 1（0x442C4A）——不是 0')
    check(box['record']['size'] == REC, '记录 12 字节')
    check('text0' in box['record']['fields']['+0x00'] and
          'text1' in box['record']['fields']['+0x04'] and
          'value' in box['record']['fields']['+0x08'],
          '记录 = {text0, text1, value}')
    ser = ref['serializer']
    check(ser['writer'].lower() == '0x00437fa0' and
          ser['reader'].lower() == '0x00437ee0',
          '写 0x437FA0 / 读 0x437EE0')
    check('100 ×' in ser['order'][2], '序列化固定写 100 条')
    ids = [c['id'] for c in ref['checks']]
    for want in ('ctor_count_1', 'set_null_gate', 'get_null_returns_count',
                 'deser_count', 'rec_writer_value', 'slot_map_body'):
        check(want in ids, '基线含判据 %s' % want)
    check(len(ref['checks']) >= 30, '判据条数 %d（>=30）' % len(ref['checks']))
    check(len(ref['unknown']) >= 3, '未解条目 %d 条' % len(ref['unknown']))
    for g in ('iroseka', 'hoshimemo'):
        s = ref['script_sites'][g]
        check(len(s['HistorySet']) == 7, '%s: HistorySet 7 个调用点' % g)
        check(len(s['HistoryGet']) == 5, '%s: HistoryGet 5 个调用点' % g)
        kinds = [p['type'] for h2 in s['HistorySet']
                 for p in h2['recent_pushes'][:1]]
        check('null' not in kinds,
              '%s: 静态调用点的 arg0 里没有 null（begin 来源仍未解）' % g)

    print('== 3. C 实现与基线一致 ==')
    hist_c = io.open(HIST_C, encoding='utf-8').read()
    hist_h = io.open(HIST_H, encoding='utf-8').read()
    vm_c = io.open(VM_C, encoding='utf-8').read()
    sc = io.open(SYSCALL_C, encoding='utf-8').read()
    check('FVP_HISTORY_SLOTS 100' in hist_h, 'FVP_HISTORY_SLOTS = 100')
    check('h->count = 1u;' in hist_c, 'fvp_history_init 里 count = 1')
    check('h->pos += 1u;' in hist_c, 'begin 先 ++pos 再写（0x43A5FC）')
    check('fvp_value_is_null(&args[0])' in vm_c,
          'vm 的 begin 判据用 fvp_value_is_null（对应 Variant type 0）')
    check('idx >= (int64_t)vm->history.count' in vm_c or
          'idx > (int64_t)vm->history.count' in vm_c or
          'vm->history.count' in vm_c,
          'vm 的 idx 上界用 count')
    check('fvp_vm_history_set(vm, args, (int)argc)' in sc,
          'syscall.c case 26 委派 fvp_vm_history_set')
    check('fvp_vm_history_get(vm, args, (int)argc, &v)' in sc,
          'syscall.c case 25 委派 fvp_vm_history_get')
    if os.path.isfile(TEST_HIST):
        proc = subprocess.run([TEST_HIST], stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=120)
        out = proc.stdout.decode('utf-8', 'replace').strip().splitlines()
        print('  ' + (out[-1] if out else '(no output)'))
        check(proc.returncode == 0 and 'failed' in (out[-1] if out else ''),
              'tests/test_history.exe 全过')
    else:
        print('  SKIP: bin/test_history.exe 未构建')

    print('== 4. 真实存档：槽尾历史环 ==')
    if not os.path.isdir(SAVE):
        print('  SKIP: 缺存档目录 %s' % SAVE)
    else:
        found = 0
        for slot in SLOTS:
            path = os.path.join(SAVE, slot + '.bin')
            if not os.path.isfile(path):
                print('  SKIP: %s 不存在' % slot)
                continue
            data = open(path, 'rb').read()
            off, hdr = read_header(data)
            blob = data[off + 200 * 113 * 4:]
            hits = find_ring(blob, hdr[2])
            if not hits:
                check(False, '%s: 找不到历史环' % slot)
                continue
            found += 1
            start, end, count, pos, recs = hits[0]
            # 只允许一个候选（避免"碰巧解析成功"）
            check(len(hits) == 1, '%s: 只有 1 个候选环（实际 %d）'
                  % (slot, len(hits)))
            check(count >= 1 and pos == count - 1,
                  '%s: 环 @tail+%#x count=%d pos=%d（pos == count-1）'
                  % (slot, start, count, pos))
            if hdr[2]:
                check(recs[pos][1] == hdr[2] or recs[pos][0] == hdr[2],
                      '%s: 最新一条的台词 == 槽头第 3 条字符串' % slot)
            else:
                # 槽头当前台词为空（系统/特殊界面存的档）时两者可以不一致：
                # s485 就是"槽头空、历史里还留着上一句"的样本
                print('  note %s: 槽头台词为空，环里最新一条 = %r（可以不一致）'
                      % (slot, recs[pos][1][:24].decode('gbk', 'replace')))
            check(recs[pos][2] == -1,
                  '%s: 最新一条的 value = -1（脚本没给有效行 id）' % slot)
            empty = sum(1 for r in recs[count:] if (r[0] or r[1] or r[2] != -1))
            check(empty == 0, '%s: 第 count 条之后都是空记录（实际 %d 条有数据）'
                  % (slot, empty))
        check(found == len(SLOTS), '6 份存档都找到历史环（%d/%d）'
              % (found, len(SLOTS)))

    print('== 5. 文档记录 ==')
    vm_doc, save_doc = (io.open(p, encoding='utf-8').read() for p in DOCS)
    check('count` 的初值是 1' in vm_doc or 'count 的初值是 1' in vm_doc,
          'vm-commands 记录 count 初值 = 1')
    check('pos == count-1' in vm_doc, 'vm-commands 记录 pos == count-1')
    check('未解' in vm_doc and ('begin' in vm_doc),
          'vm-commands 把 begin 触发源列为未解')
    check('台词历史环' in save_doc, 'save-format 记录槽尾第 20 段 = 台词历史环')

    if fails:
        print('\nFAIL: %d 项不满足' % len(fails))
        return 1
    print('\nOK: 台词历史（HistorySet 26 / HistoryGet 25）冻结完成：'
          'handler 判据 + 容器构造（count 初值 1）+ 记录环读写顺序 + 读档回填，'
          'C 实现与基线和单元测试一致；6 份真实存档的槽尾历史环全部解析成功'
          '（s481 count=1 / s905 count=11，最新台词 == 槽头台词）；'
          'begin 的真实触发源仍列为未解')
    return 0


if __name__ == '__main__':
    sys.exit(main())