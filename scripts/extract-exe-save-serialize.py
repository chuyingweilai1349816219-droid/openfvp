#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""从 exe 原版程序里冻结「存档槽序列化器」的写入顺序（0x43DE90）。

为什么：读档续玩 step-2 要把槽尾的快照**逐段回灌**，前提是知道
「哪一段来自引擎的哪个对象、长度由什么决定」。之前的 `save-format.md`
§2.4.4 是从调用序列**人工读出来**的，标注了大量"推定"；本脚本把每条
写入指令的**地址 + 判据字节**冻结成基线，门禁重跑时逐条核对，指令流一
变就报错（不会悄悄漂移）。

产物：`ref/exe-save-serialize.json`
门禁：`tests/verify_exe_save_serialize.py`

用法：
    python scripts/extract-exe-save-serialize.py            # 打印 + 写 json
    python scripts/extract-exe-save-serialize.py --check    # 只比对（门禁用）
"""
import argparse
import hashlib
import io
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(r'C:\Users\Mechrevo\Downloads', '五彩斑斓的世界HD',
                   'iroseka_HD.exe')
OUT_JSON = os.path.join(ROOT, 'ref', 'exe-save-serialize.json')
BASE = 0x400000

# 序列化器本体与它用的写缓冲助手
SERIALIZER = {
    'fn': '0x0043de90',
    'end': '0x0043e67b',
    'end_bytes': 'c414c3',
    'note': '整个 sNNN.bin 的序列化器（时间戳 → 三条字符串 → 缩略图 → 槽尾快照）',
}

HELPERS = {
    'buffer_struct': {
        'fields': ['+0x0 数据指针', '+0x4 容量', '+0x8 已写长度', '+0xc 扩容粒度'],
        'note': 'engine 对象自身：esi = this，缓冲区就在 engine+0x00 起',
    },
    'grow': {
        'fn': '0x00434060',
        'bytes': '56578b7c240c',
        'note': '按粒度扩容（idiv 粒度后 imul 回去）',
    },
    'append_raw': {
        'fn': '0x004343c0',
        'bytes': '568bf18b4e08',
        'note': 'append(ptr, len)：先确保容量，再 memcpy，再加长已写长度',
    },
    'string_u16': {
        'fn': '0x00434410',
        'bytes': '538bd98b03',
        'must_contain': ['3d64134800'],
        'empty_sentinel': '0x00481364',
        'note': 'u16 长度（**含结尾 NUL**）+ 字节；指针等于 0x481364 的空串只写 u16 0',
    },
    'memcpy': {'fn': '0x00447800', 'note': 'memcpy（所有定长段的搬运）'},
    'alloc': {'fn': '0x004470eb', 'note': 'operator new（缩略图临时缓冲）'},
}

# 每条：order = 写入顺序；stage=preamble（缩略图之前的文件头）/ tail（槽尾快照）
SEGMENTS = [
    # ---- 文件头：时间戳 / 三条字符串 / 缩略图 ----
    {'order': 1, 'stage': 'preamble', 'label': 'timestamp',
     'kind': 'inline', 'va': '0x0043dea5', 'size': 7,
     'frags': ['ff15c8d04500', '66893c02'],
     'layout': 'u16 年 + 1 字节月 + 日 + 星期 + 时 + 分（**没有秒**）',
     'src': 'GetLocalTime（IAT 0x45d0c8）栈上 SYSTEMTIME',
     'note': '7 字节与实测槽头一致'},
    {'order': 2, 'stage': 'preamble', 'label': 'string[0]',
     'kind': 'string_u16', 'va': '0x0043dfcd', 'src': 'engine+0xfb4',
     'frags': ['8b86b40f0000'],
     'layout': 'u16 长度（**不含 NUL**）+ 字节',
     'note': 'strlen 后 len=strlen，再 0x4343C0 追加，**不写结尾 NUL**'},
    {'order': 3, 'stage': 'preamble', 'label': 'string[1]',
     'kind': 'string_u16', 'va': '0x0043e035', 'src': 'engine+0xfb8',
     'frags': ['8b86b80f0000'], 'note': '同上（长度不含 NUL）'},
    {'order': 4, 'stage': 'preamble', 'label': 'string[2]',
     'kind': 'string_u16', 'va': '0x0043e098', 'src': 'engine+0xfbc',
     'frags': ['8b86bc0f0000'], 'note': '同上（长度不含 NUL）'},
    {'order': 5, 'stage': 'preamble', 'label': 'thumbnail',
     'kind': 'blob', 'va': '0x0043e0f9', 'src': '窗口对象 engine+0x8c',
     'frags': ['8b86d00f0000', '0faf86d40f0000'],
     'size': 'w*h*4（engine+0xfd0 × engine+0xfd4，实测 200×113×4）',
     'note': '0x4255C0 抓窗口像素 → 0x447800 拷进缓冲'},

    # ---- 槽尾快照（以下全部按顺序写进缩略图之后的 tail）----
    {'order': 6, 'stage': 'tail', 'label': 'per-slot globals',
     'kind': 'array', 'va': '0x0043e17f',
     'frags': ['8bbec8666a00', '8b9ebcdf6800'],
     'src': 'engine+0x68dfbc', 'count_src': 'engine+0x6a66c8',
     'stride': 8,
     'note': '每槽全局变量：count × 8B，count 是**运行时**条数。6 份真实存档实测 '
             'count = 1471（0x2DF8 字节），所以它后面 128 槽字符串表、0x100 块…'
             '直到字符串池标签的偏移都随 count 平移（标签 = 0x16610 + count*8，'
             '样本里 = 0x19408）。旧文档写成 count=0 是错的，已按实测改正'},
    {'order': 7, 'stage': 'tail', 'label': 'coroutine snapshots',
     'kind': 'array_call', 'va': '0x0043e1cb',
     'frags': ['8dbec0df6800', 'bb20000000'],
     'src': 'engine+0x68dfc0', 'count': 32, 'stride': 0xb28,
     'call': '0x00445640',
     'note': '32 个协程上下文（写死 0x20 条、步长 0xB28）'},
    {'order': 8, 'stage': 'tail', 'label': 'current thread index',
     'kind': 'u32', 'va': '0x0043e20a', 'frags': ['8b86d0666a00'],
     'src': 'engine+0x6a66d0', 'size': 4,
     'note': '线程下标（读档重启时被归零的那个字段）'},
    {'order': 9, 'stage': 'tail', 'label': 'u32 at 0x6a49e8',
     'kind': 'u32', 'va': '0x0043e21a', 'frags': ['8bbee8496a00'],
     'src': 'engine+0x6a49e8', 'size': 4, 'note': '语义未解'},
    {'order': 10, 'stage': 'tail', 'label': 'dynamic string table',
     'kind': 'string_array', 'va': '0x0043e24c',
     'frags': ['8dbeec496a00', 'bb80000000'],
     'src': 'engine+0x6a49ec', 'count': 0x80, 'stride': 4,
     'writer': '0x00434410',
     'note': '128 个 char* 槽，逐条 u16 长度（**含结尾 NUL**，0x434410 的语义）'
             '+ 字节；空槽只写 2 字节 0'},
    {'order': 11, 'stage': 'tail', 'label': 'block 0x6a4bec',
     'kind': 'blob', 'va': '0x0043e266', 'frags': ['8d96ec4b6a00'],
     'src': 'engine+0x6a4bec', 'size': 0x100, 'note': '定长 256B，语义未解'},
    {'order': 12, 'stage': 'tail', 'label': 'block 0x6a4cec',
     'kind': 'blob', 'va': '0x0043e2cc', 'frags': ['8d8eec4c6a00'],
     'src': 'engine+0x6a4cec', 'size': 0x100, 'note': '定长 256B，语义未解'},
    {'order': 13, 'stage': 'tail', 'label': 'u32 at 0x6a4dec',
     'kind': 'u32', 'va': '0x0043e2e2', 'frags': ['8bbeec4d6a00'],
     'src': 'engine+0x6a4dec', 'size': 4, 'note': '语义未解'},
    {'order': 14, 'stage': 'tail', 'label': 'table 0x6a4df0',
     'kind': 'array_call', 'va': '0x0043e317',
     'frags': ['8dbef04d6a00'], 'src': 'engine+0x6a4df0',
     'count': 0x80, 'stride': 0x14, 'call': '0x00445330',
     'note': '128 × 20B（子序列化器 0x445330 决定实际字节数）'},
    {'order': 15, 'stage': 'tail', 'label': 'block 0x6a61f0',
     'kind': 'blob', 'va': '0x0043e330', 'frags': ['8d86f0616a00'],
     'src': 'engine+0x6a61f0', 'size': 0x200, 'note': '定长 512B'},
    {'order': 16, 'stage': 'tail', 'label': 'block 0x6a63f0',
     'kind': 'blob', 'va': '0x0043e36f', 'frags': ['8d96f0636a00'],
     'src': 'engine+0x6a63f0', 'size': 0x200, 'note': '定长 512B'},
    {'order': 17, 'stage': 'tail', 'label': 'byte 0x6a65fc+0xc0',
     'kind': 'u8', 'va': '0x0043e3ac', 'frags': ['8dbefc656a00', '8a97c0000000'],
     'src': 'engine+0x6a65fc+0xc0', 'size': 1, 'note': '单一字节，语义未解'},
    {'order': 18, 'stage': 'tail', 'label': 'table 0x6a65fc',
     'kind': 'array_call', 'va': '0x0043e3f3', 'frags': ['e8d8a2ffff'],
     'src': 'engine+0x6a65fc', 'count': 0x10, 'stride': 0xc,
     'call': '0x004386d0', 'note': '16 × 12B'},
    {'order': 19, 'stage': 'tail', 'label': 'palette',
     'kind': 'blob', 'va': '0x0043e3ff',
     'frags': ['81c100040000', '8d96c0446a00'],
     'src': 'engine+0x6a44c0', 'size': 0x400, 'note': '256 × BGRA（ColorSet 写的调色板）'},
    {'order': 20, 'stage': 'tail', 'label': 'history ring',
     'kind': 'call', 'va': '0x0043e448',
     'frags': ['568d8ec0496a00', 'e8cc9cffff'],
     'src': 'engine+0x6a49c0', 'call': '0x00438120',
     'note': '台词历史环（container = count/pos/base，100 条 × 12B 记录，'
             '每条写 {strlen(text0), text0, strlen(text1), text1, i32 value}）；'
             '前两版文档把它写成"全局变量 + flag 位图"是错的，见 '
             'ref/exe-history.json'},
    {'order': 21, 'stage': 'tail', 'label': 'gaiji table',
     'kind': 'call', 'va': '0x0043e454',
     'frags': ['568d8ecc496a00', 'e880f8ffff'],
     'src': 'engine+0x6a49cc', 'call': '0x0043dce0',
     'note': '外字表（变长）'},
    {'order': 22, 'stage': 'tail', 'label': 'object array 0x101064',
     'kind': 'array_call', 'va': '0x0043e460',
     'frags': ['8dbe64101000', 'bb00100000'],
     'src': 'engine+0x101064', 'count': 0x1000, 'stride': 0x51c,
     'call': '0x004292b0',
     'note': '4096 × 0x51C 的对象数组（每项变长：标志字节 + 条件字符串）'},
    {'order': 23, 'stage': 'tail', 'label': 'object array 0x61d064',
     'kind': 'array_call', 'va': '0x0043e482',
     'frags': ['8dbe64d06100', 'bb40000000'],
     'src': 'engine+0x61d064', 'count': 0x40, 'stride': 0x34,
     'call': '0x00429570', 'note': '64 × 0x34'},
    {'order': 24, 'stage': 'tail', 'label': 'prim array',
     'kind': 'blob', 'va': '0x0043e49f',
     'frags': ['81c100400300', '8d9664dd6100'],
     'src': 'engine+0x61dd64', 'size': 0x34000,
     'note': '4096 × 0x34 图元数组原样拷贝（长度写死 0x34000）'},
    {'order': 25, 'stage': 'tail', 'label': 'tween managers',
     'kind': 'call_chain', 'va': '0x0043e4e8',
     'frags': ['568d8e701d6500', '568d8e041e6500',
               '568d8e0c386500', '568d8e54ba6500',
               '568d8e5c9a6700', '568d8e64ce6700',
               '568d8e6c0a6800', '568d8e743e6800',
               '568d8e043f6800'],
     'calls': ['0x00426410', '0x00426830', '0x00426cd0', '0x00426f40',
               '0x00427750', '0x004273b0', '0x00427750', '0x004287d0',
               '0x00428020'],
     'src': ['engine+0x651d70', 'engine+0x651e04', 'engine+0x65380c',
             'engine+0x65ba54', 'engine+0x679a5c', 'engine+0x67ce64',
             'engine+0x680a6c', 'engine+0x683e74', 'engine+0x683f04'],
     'note': '9 个补间/动画管理器（位置/透明度/帧动画/移动…），各自变长'},
    {'order': 26, 'stage': 'tail', 'label': 'u32 at 0x651d64',
     'kind': 'u32', 'va': '0x0043e554', 'frags': ['8b86641d6500'],
     'src': 'engine+0x651d64', 'size': 4, 'note': '语义未解'},
    {'order': 27, 'stage': 'tail', 'label': 'slots engine+0x98',
     'kind': 'array_call', 'va': '0x0043e587',
     'frags': ['bf98000000', '81ff18010000'],
     'src': 'engine+0x98', 'count': 0x20, 'stride': 4,
     'call': '0x0042d880',
     'note': 'edi 从 0x98 步进 4 到 0x114（32 项），每项一个子序列化器'},
    {'order': 28, 'stage': 'tail', 'label': 'window +0x2d4',
     'kind': 'call', 'va': '0x0043e5aa',
     'frags': ['8b828c000000', '8b88d4020000'],
     'src': '窗口对象 +0x2d4', 'call': '0x004221e0',
     'note': '变长（含 1 字节标志 + 可选字符串）'},
    {'order': 29, 'stage': 'tail', 'label': 'script engine walk (9 objects)',
     'kind': 'call_chain', 'va': '0x0043e5c2',
     'frags': ['8b89347e6a00', '81c178436800', '81c1c0936800'],
     'calls': ['0x00441320'],
     'src': ['engine+0x6a7e34', 'engine+0x684378', 'engine+0x6893c0'],
     'note': '0x441320 里对 256 个子对象各调一次 0x441130；'
             '随后两个 0x429EA0 子序列化器'},
    {'order': 30, 'stage': 'tail', 'label': 'byte at 0x458',
     'kind': 'u8', 'va': '0x0043e5f8', 'frags': ['0fb69a58040000'],
     'src': 'engine+0x458', 'size': 1, 'note': '末尾两个字节之一（实测 01）'},
    {'order': 31, 'stage': 'tail', 'label': 'byte at 0x85 != 0',
     'kind': 'u8_bool', 'va': '0x0043e62f',
     'frags': ['389a85000000', '0f95c3'],
     'src': 'engine+0x85', 'size': 1,
     'note': '末尾两个字节之二：写成 (engine+0x85 != 0) 的 0/1（实测 01）'},
]

# 读档侧（反序列化）的游标顺序：地址 + 判据字节 + 语义
# 实现在 0x43EE90 里内联：0x43EFCC 起把游标置 7（跳过 7 字节时间戳），然后按
# 与写档完全相同的顺序读回，每段尺寸取自同一个引擎字段。
READER = [
    {'order': 1, 'va': '0x0043efcc', 'frags': ['8b06', 'c7460807000000'],
     'fact': '游标 = 7（跳过 7 字节时间戳，不解析）'},
    {'order': 2, 'va': '0x0043efd5', 'frags': ['0fbf4007'],
     'fact': '头 3 条字符串：u16 长度（不含 NUL）+ 字节'},
    {'order': 3, 'va': '0x0043f0c7',
     'frags': ['8b8ed40f0000', '0faf8ed00f0000'],
     'fact': '缩略图：游标 += w*h*4（engine+0xFD0/0xFD4）'},
    {'order': 4, 'va': '0x0043f0d4', 'frags': ['8b86c8666a00', '03c0', '03c0'],
     'fact': '每槽全局变量：条数 = engine+0x6A66C8，字节数 = 条数×8'},
    {'order': 5, 'va': '0x0043f100', 'frags': ['8dbed4e76800', 'bd20000000'],
     'fact': '32 个协程快照：步长 0xB28，逐条调 0x445540（变长，不是整块 memcpy）'},
    {'order': 6, 'va': '0x0043f12a', 'frags': ['8b0e', '034e08', '8b11'],
     'fact': 'u32 → engine+0x6A66D0（线程下标）'},
    {'order': 7, 'va': '0x0043f13f', 'frags': ['8b4608', '8b0e', '8b0c08'],
     'fact': 'u32 → engine+0x6A49E8'},
    {'order': 8, 'va': '0x0043f152', 'frags': ['8dbee c496a00'.replace(' ', ''),
                                              '8d6b7c'],
     'fact': '128 槽字符串表：逐条调 0x433F60（u16 长度含 NUL）'},
    {'order': 9, 'va': '0x0043f176', 'frags': ['6800010000', '8d96ec4b6a00'],
     'fact': '0x100 块 → engine+0x6A4BEC'},
    {'order': 10, 'va': '0x0043f182', 'frags': ['81c100010000'],
     'fact': '游标 += 0x100（第二块 → engine+0x6A4CEC）'},
    {'order': 11, 'va': '0x0043f1b3', 'frags': ['8b0c08', '898eec4d6a00'],
     'fact': 'u32 → engine+0x6A4DEC'},
    {'order': 12, 'va': '0x0043f1ca', 'frags': ['8dbef04d6a00', 'bd00010000'],
     'fact': '128 项表：逐项调 0x446210（变长：3×u32 + count×12）'},
    {'order': 13, 'va': '0x0043f1e5', 'frags': ['6800020000', '8d96f0616a00'],
     'fact': '0x200 块 → engine+0x6A61F0'},
    {'order': 14, 'va': '0x0043f207', 'frags': ['6800020000', '8d86f0636a00'],
     'fact': '0x200 块 → engine+0x6A63F0'},
    {'order': 15, 'va': '0x0043f22c', 'frags': ['8d8efc656a00', 'e83895ffff'],
     'fact': '1 字节 + 16 项（0x438770）→ engine+0x6A65FC'},
    {'order': 16, 'va': '0x0043f238', 'frags': ['81c100040000'],
     'fact': '游标 += 0x400（调色板）'},
    {'order': 17, 'va': '0x0043f24e', 'frags': ['8d8ec0446a00'],
     'fact': '调色板 → engine+0x6A44C0'},
    {'order': 18, 'va': '0x0043f262', 'frags': ['8b02', '8986c0496a00'],
     'fact': '读档回填历史环：u32 count → engine+0x6A49C0'},
    {'order': 19, 'va': '0x0043f272', 'frags': ['8b1401', '8996c4496a00'],
     'fact': 'u32 pos → engine+0x6A49C4'},
    {'order': 20, 'va': '0x0043f282', 'frags': ['8b8ec8496a00', '81ffb0040000'],
     'fact': '100 条历史记录：逐条调 0x437EE0'},
]

# 静态分析阶段读到的、数据侧可以验证的推论（门禁会用真实存档核对）
INVARIANTS = [
    {'id': 'globals_first',
     'claim': 'tail 从 +0 起就是"每槽全局变量"表：条数 = engine+0x6A66C8，'
              '每条 8 字节 {u8 type, 3B pad, u32 value}；6 份样本条数都是 1471，'
              '共 0x2DF8 字节，type 只出现 0/1/2/4/5/6',
     'verify': '6/6 份存档 tail[0:0x2DF8] 按 8 字节切分后 type ∈ {0,1,2,4,5,6}，'
               '且 tail[0x18]（第 4 条记录的 type）恒为 2'},
    {'id': 'string_pool_after_globals',
     'claim': '字符串池标签 = 0x16610 + 全局变量字节数；样本里 = 0x16610 + 1471×8 '
              '= 0x19408 —— **不是固定偏移**',
     'verify': '6/6 份存档在 0x19408 找到 u32 标签 0x20 且条数 ≤ 8；'
               'src/savetail.c 已改成扫描定位'},
    {'id': 'tail_end_marker',
     'claim': '文件最后两字节 = (engine+0x458, engine+0x85!=0)',
     'verify': '实测 6 份存档末两字节都是 01 01'},
    {'id': 'head_marker',
     'claim': 'tail+0x18 固定为 02 —— 那是第 4 条全局变量记录的 type（int），'
              '不是旧文档以为的"协程快照 0 的字段"',
     'verify': '实测 6 份存档都是 02'},
]

UNKNOWN = [
    '各子序列化器（0x438120 / 0x43DCE0 / 0x4292B0 / 0x429570 / 0x42D880 / '
    '0x4221E0 / 0x441320 / 0x426410 系列 / 0x445330 / 0x445640 / 0x4386D0）'
    '的**字节数**：段顺序已确证，但每段多长仍要逐个子序列化器逆',
    'tail 中段的绝对偏移：现在只能前向走到 0x16608（128 槽字符串表末尾），'
    '之后的段因为子序列化器长度未知，还无法给固定偏移',
    'engine+0x6a49e8 / 0x6a4dec / 0x6a65fc+0xc0 / 0x651d64 / 0x458 / 0x85 的语义',
]


def load_exe(path):
    data = open(path, 'rb').read()
    pe = struct.unpack_from('<I', data, 0x3C)[0]
    nsec = struct.unpack_from('<H', data, pe + 6)[0]
    opt = pe + 24
    sec_off = opt + struct.unpack_from('<H', data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        off = sec_off + i * 40
        name = data[off:off + 8].rstrip(b'\0').decode('latin1')
        vsize, vaddr, rsize, roff = struct.unpack_from('<IIII', data, off + 8)
        secs.append((name, vaddr, vsize, roff, rsize))
    return data, secs


def foff(secs, va):
    rva = va - BASE
    for _n, vaddr, vsize, roff, rsize in secs:
        if vaddr <= rva < vaddr + max(vsize, rsize):
            return roff + (rva - vaddr)
    raise SystemExit('VA %x not mapped' % va)


def u8(data, secs, va, n):
    fo = foff(secs, va)
    return data[fo:fo + n]


def check_frags(blob, frags):
    """frags 里每个片段都必须出现在 blob 中；片段内允许 [xx] 通配一个字节。"""
    import re
    text = blob.hex()
    for frag in frags:
        pat = frag.replace('[', '.').replace(']', '')
        if re.search(pat, text) is None:
            return False, frag
    return True, None


def reader_checks(data, secs):
    """逐条校验读档侧判据字节，返回冻结清单。"""
    out = []
    for item in READER:
        blob = u8(data, secs, int(item['va'], 16), 0x40)
        ok, bad = check_frags(blob, item['frags'])
        if not ok:
            raise SystemExit('FAIL: reader #%d @%s 判据字节 %s 不符（实际 %s）'
                             % (item['order'], item['va'], bad, blob.hex()))
        rec = dict(item)
        rec['bytes'] = blob.hex()
        out.append(rec)
    return out


def extract(path):
    data, secs = load_exe(path)
    out_segments = []
    for seg in SEGMENTS:
        va = int(seg['va'], 16)
        blob = u8(data, secs, va, 0x120)
        ok, bad = check_frags(blob, seg['frags'])
        if not ok:
            raise SystemExit('FAIL: %s @%s 找不到判据字节 %s（实际 %s）'
                             % (seg['label'], seg['va'], bad, blob.hex()))
        rec = dict(seg)
        rec['bytes'] = blob.hex()
        out_segments.append(rec)

    helpers = {}
    for key in ('grow', 'append_raw', 'string_u16'):
        h = dict(HELPERS[key])
        va = int(h['fn'], 16)
        blob = u8(data, secs, va, 0x20)
        frags = [h['bytes']] + list(h.get('must_contain', []))
        ok, bad = check_frags(blob, frags)
        if not ok:
            raise SystemExit('FAIL: helper %s @%s 判据字节 %s 不符（实际 %s）'
                             % (key, h['fn'], bad, blob.hex()))
        h['bytes'] = blob.hex()
        helpers[key] = h

    body = u8(data, secs, int(SERIALIZER['fn'], 16),
              int(SERIALIZER['end'], 16) - int(SERIALIZER['fn'], 16) + 1)
    tail = body[-3:].hex()
    if tail != SERIALIZER['end_bytes']:
        raise SystemExit('FAIL: 序列化器末尾字节 %s != %s'
                         % (tail, SERIALIZER['end_bytes']))

    return {
        'exe': {'sha256': hashlib.sha256(data).hexdigest(),
                'size': len(data), 'image_base': '0x%08x' % BASE},
        'serializer': dict(SERIALIZER,
                           size=len(body),
                           sha256=hashlib.sha256(body).hexdigest()),
        'helpers': helpers,
        'segments': out_segments,
        'tail_starts_after': 'thumbnail（第 5 段）之后',
        'invariants': INVARIANTS,
        'reader': reader_checks(data, secs),
        'unknown': UNKNOWN,
    }


def main(argv=None):
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    opt = ap.parse_args(argv)
    if not os.path.isfile(EXE):
        print('SKIP: %s 不存在' % EXE)
        return 0
    result = extract(EXE)
    text = json.dumps(result, ensure_ascii=False, indent=1,
                      sort_keys=True) + '\n'
    print('serializer %s..%s  %d 字节  sha256 %s'
          % (result['serializer']['fn'], result['serializer']['end'],
             result['serializer']['size'],
             result['serializer']['sha256'][:16]))
    for seg in result['segments']:
        print('  #%-2d %-6s %-30s %s'
              % (seg['order'], seg['stage'], seg['label'], seg.get('src')))
    if opt.check:
        if not os.path.isfile(OUT_JSON):
            print('FAIL: %s 不存在' % OUT_JSON)
            return 1
        old = io.open(OUT_JSON, encoding='utf-8').read()
        if old != text:
            print('FAIL: 提取结果与 ref/exe-save-serialize.json 不一致')
            import difflib
            for line in list(difflib.unified_diff(
                    old.splitlines(), text.splitlines(), 'ref', 'new',
                    lineterm=''))[:40]:
                print(line)
            return 1
        print('OK: 与 ref/exe-save-serialize.json 一致')
        return 0
    io.open(OUT_JSON, 'w', encoding='utf-8', newline='\n').write(text)
    print('写入 %s' % OUT_JSON)
    return 0


if __name__ == '__main__':
    sys.exit(main())