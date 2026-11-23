# C 资源层：BIN 包 / inflate / HZC1 图像

> 目标：给 `fvp/src` 的 C 引擎补上「资源层」——读 BIN 包、解 zlib、解 hzc1 图像。
> 全部 C99、只用标准库、**不依赖 zlib**（w64devkit 里没有），也不引入任何第三方源码。
> 验收与 Python 参考实现逐字节/逐像素等价，见第 6 节。
>
> 相关文档：`docs/00-engine.md`（BIN/HCB 格式）、`docs/hzc1-format.md`（HZC1 格式）、
> `docs/01-assets.md`（资源包清单）。

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

## 0. 结论速览

| 项 | 结果 |
|---|---|
| BIN | `graph.bin` 1112 条 + 其余 11 个包 20613 条，共 **21725 条**索引/名字/大小/偏移与 Python 逐条一致 |
| inflate | `graph.bin` 全库 **1112/1112** 个 zlib 流（共 2,539,861,820 字节）与 Python `zlib.decompress` 逐字节相同；随机 fuzz **125,000** 次零崩溃、零越界 |
| HZC1 | **1112/1112** 张图、3654 帧、**727,573,943** 像素与 `work/hzc1.py` 逐像素比对 **0 差异**，fmt 0/1/2/3/4 全覆盖 |
| 性能 | 1920×1080 BGRA32 一张 **10.6–12.2 ms**（含 inflate + 转 RGBA）；纯 inflate 约 **1 GiB/s** |

## 1. 文件清单

| 文件 | 作用 |
|---|---|
| `src/bin.h` / `src/bin.c` | BIN 包读取：索引常驻、数据按需读、CP932→UTF-8 名字 |
| `src/inflate.h` / `src/inflate.c` | 手写 DEFLATE（RFC1951）+ zlib 包装（RFC1950，含 Adler-32 校验） |
| `src/hzc1.h` / `src/hzc1.c` | hzc1/NVSG 头解析 + 五种像素格式解码为 RGBA8888 |
| `tests/c_assets.c` | 验收/调试工具（list / dump / hdr / rgba / png(ppm) / inflate / fuzz / bench） |
| `tests/verify_assets.ps1` | 一键：编译 + 跑全部验收 |
| `tests/verify_assets.py` | 验收本体（对 Python 参考做逐字节/逐像素比对） |

构建产物：`bin/c_assets.exe`（工具）、`out/verify/`（验收临时文件）。
`src/bin.c` 复用 `src/gen_cp932.h` 的 CP932 表（与 `src/encoding.c` 同一份数据），
因此 C VM 与资源层解出的名字完全一致；`gen_*.h` 未被修改。

## 2. 构建

```powershell
# 一键验收（会先编译）
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1

# 只编译工具
$env:PATH = "tools\w64devkit\w64devkit\bin;$env:PATH"
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv -Ifvp\src `
    -o fvp\bin\c_assets.exe `
    fvp\tests\c_assets.c fvp\src\bin.c fvp\src\inflate.c fvp\src\hzc1.c
```

`verify_assets.ps1` 开关：`-Quick`（小样本）、`-Full`（全部 1112 个文件）、
`-Game <dir>`、`-Extract <dir>`、`-Seed <n>`、`-NoBuild`。
Python 解释器默认用 DSH 自带的那个，可用环境变量 `FVP_PYTHON` 覆盖。

## 3. BIN 包（`src/bin.h`）

包格式（小端）：

```
+0x00 u32 file_count
+0x04 u32 name_table_len
+0x08 file_count × { u32 name_off, u32 data_off, u32 size }
      名字表（Shift-JIS，NUL 结尾；name_off 相对名字表起点）
      数据区（data_off / size 是文件内绝对偏移）
```

| 接口 | 说明 |
|---|---|
| `BinArchive *bin_open(const char *path)` | 打开并解析索引+名字表；失败返回 `NULL`，原因见 `bin_last_error()` |
| `void bin_close(BinArchive *)` | 关闭，释放索引、名字表与名字缓存 |
| `uint32_t bin_count(const BinArchive *)` | 条目数 |
| `uint32_t bin_name_table_len(...)` / `uint64_t bin_names_start(...)` / `uint64_t bin_data_start(...)` | 名字表长度 / 名字表起点（`8+12N`）/ 数据区起点（`8+12N+名字表长度`） |
| `const char *bin_name(const BinArchive *, uint32_t i)` | 第 i 条名字，**UTF-8**，首次访问时解码并缓存 |
| `const uint8_t *bin_name_raw(..., size_t *len)` | 名字原始 Shift-JIS 字节（不含 NUL） |
| `uint32_t bin_size(...)` / `uint64_t bin_offset(...)` | 条目大小 / 数据绝对偏移 |
| `int bin_read(const BinArchive *, uint32_t i, void *buf, size_t buflen)` | 读整条数据；`buflen < size` 返回 `BIN_ERR_SMALL` |
| `void *bin_read_alloc(const BinArchive *, uint32_t i, size_t *size_out)` | 读进新 malloc 的缓冲（多留 1 字节 NUL，`free()` 释放） |
| `int bin_find(const BinArchive *, const char *utf8_name)` | 按名字查索引，返回 `-1` 表示没有 |
| `int bin_sjis_to_utf8(const uint8_t *, size_t, char *, size_t)` | 独立的 CP932→UTF-8；非法序列→U+FFFD（消费 1 字节），与 Python `decode('cp932','replace')` 一致 |
| `const char *bin_last_error(void)` / `bin_strerror(int)` | 最近一次失败原因 / 错误码文本 |

设计要点：

* **索引常驻、数据按需**：打开时只读 8 字节头 + `12N` 索引 + 名字表
  （`voice.bin` 18612 条约 0.5 MB），不整包加载（`graph_vis1.bin` 有 1.99 GB）。
* 打开时逐条校验 `name_off < name_table_len`、`data_off+size <= 文件大小`，
  损坏包直接报错而不是越界。
* 数据读取用 64 位文件偏移（Windows `_fseeki64`，POSIX 走 `fseek` 并拒绝 >2 GB）。
* 名字解码用 `src/gen_cp932.h` 的表，流式规则与 `encoding.c` 的 `fvp_decode()` 相同；
  本作 1112 条名字里只有 1 条非 ASCII（`x禁止`），已验证一致。
* 单线程（错误信息是全局静态缓冲）；索引/名字表在 `bin_close` 前一直有效。

## 4. inflate（`src/inflate.h`）

```c
int fvp_inflate(const uint8_t *src, size_t srclen, uint8_t *dst, size_t dstlen);
int fvp_inflate_zlib(...);   /* 强制要求 zlib 包装 */
int fvp_inflate_raw(...);    /* 强制裸 deflate 流 */
uint32_t fvp_adler32(uint32_t adler, const void *data, size_t len);
const char *fvp_inflate_strerror(int code);
```

* 返回**解出的字节数**（≥0），失败返回负错误码：
  `ERR_ARG/-1`、`ERR_HEADER/-2`（zlib 头坏或需要预置字典）、`ERR_TRUNC/-3`、
  `ERR_DATA/-4`、`ERR_BUF/-5`（输出超过 `dstlen`）、`ERR_CHECKSUM/-6`、`ERR_MEMORY/-7`。
* `fvp_inflate()` 自动识别：前两字节满足 `CM==8` 且 `(CMF*256+FLG)%31==0` 就当 zlib，
  否则当裸 deflate。已知歧义：若裸流前两字节碰巧是合法 zlib 头会被误判，
  此时用 `fvp_inflate_raw()`。
* 只支持单条流、一次解完；`dstlen` 必须给足（多出即报错，不静默截断）。
* 支持 stored / fixed Huffman / dynamic Huffman 三种块，多块流、空流、0 字节输出均可。
* zlib 包装会校验 Adler-32；裸流无校验。
* 实现：64 位 LSB-first 位读取器 + 两级 Huffman 表（9 位根表 + 6 位子表），
  输入尾部不足 15 位时自动切换到逐位 canonical 解码；所有输入/输出访问都做边界检查。

## 5. HZC1（`src/hzc1.h`）

```c
int hzc1_parse(const uint8_t *buf, size_t len, Hzc1Info *info);
int hzc1_decode_frame(const Hzc1Info *info, uint32_t index, uint8_t *rgba_out);
int hzc1_decode_all(const Hzc1Info *info, uint8_t *rgba_out);
const char *hzc1_format_name(int fmt);
const char *hzc1_strerror(int code);
```

44 字节头字段映射（详见 `docs/hzc1-format.md`）：

| 偏移 | 字段 | `Hzc1Info` |
|---|---|---|
| 0x04 | zlib 解压后字节数 | `uncompressed_size` |
| 0x10 | 未知常量（1112/1112 = 0x0100） | `unknown_0x10` |
| 0x12 | 像素格式 0..4 | `fmt` |
| 0x14/0x16 | 宽 / 单帧高 | `width` / `height` |
| 0x18/0x1A | 游戏内屏幕坐标 | `x` / `y` |
| 0x20 | 帧数（仅 fmt=2 有效） | `frame_count_field`、`frames` |
| 0x2C.. | zlib 流 | `payload` / `payload_len`（指向调用方缓冲） |

`hzc1_parse` 校验：`hzc1`/`NVSG` 魔数、fmt∈0..4、宽高非 0、帧数合理、
`uncompressed_size == width*height*bpp*frames`（本作 1112/1112 全部满足）。

`hzc1_decode_frame` 输出 `width*height*4` 字节 RGBA8888（行序 top-down）：

| fmt | 文件内格式 | 转 RGBA |
|---|---|---|
| 0 | BGR24 | `R,G,B = src[2],src[1],src[0]`，A=255 |
| 1 | BGRA32 | `R,G,B,A = src[2],src[1],src[0],src[3]` |
| 2 | BGRA32 多帧纵向堆叠 | 同上；frame i 起点 `(0, i*height)` |
| 3 | Gray8 | `R=G=B=v`，A=255 |
| 4 | 1bit 掩码（1 字节/像素） | `v = (uint8_t)(byte*255)`（重实现 numpy uint8 回绕语义），`R=G=B=v`，A=255 |

`hzc1_decode_all` 一次 inflate、按顺序解全部帧（`frames*width*height*4` 字节）。
每次 `hzc1_decode_frame` 都会重新 inflate 整条流；只解一帧时用 `decode_frame`，
要连续播放多帧用 `decode_all` 更划算（多帧文件占总帧数 3654/1112，值得一次解完）。

内存峰值：raw 缓冲 = `uncompressed_size`（`graph.bin` 最大 17,230,500 字节，
`0558_EF_syouki` 2625×1641 BGRA），另加调用方提供的 RGBA 输出缓冲。

### 验收工具 `c_assets`

```
c_assets list <包>                       列出条目数/名字/大小/偏移
c_assets find <包> <UTF-8 名字>          按名字查索引（-1 = 没有）
c_assets findall <包> <名字列表.txt>      逐行查索引
c_assets dump <包> <索引> <输出>         导出条目原始字节
c_assets hdr <hzc1>                      打印头字段（单行 key=value）
c_assets rgba <hzc1> <输出> [帧|all]      导出 RGBA8888
c_assets png <hzc1> <输出.ppm> [帧]       导出 PPM(P6)，默认第 0 帧（别名叫 ppm）
c_assets inflate <输入> <输出> <期望长度> [偏移]       zlib/裸流自动识别
c_assets rawinflate <输入> <输出> <期望长度> [偏移]    强制裸 deflate
c_assets fuzz <次数> <种子> [种子文件列表]             随机/变异/截断输入 fuzz
c_assets bench <hzc1> <次数>                            解码计时
c_assets bench-inflate <输入> <期望长度> <次数> [偏移]  纯 inflate 计时
```

退出码：0 成功、1 失败、2 用法错误。`list` 的行格式是
`索引\t大小\t偏移\t名字`（名字放最后，因为名字里可能有 Tab），前 4 行是
`count/nametab/names_start/data_start`。

## 6. 与 Python 参考实现的对应关系

| C | Python | 说明 |
|---|---|---|
| `bin_open` / `bin_name` / `bin_size` / `bin_offset` | `bin_archiver` 的 `extract()` 索引部分 | 头 8 字节、`12N` 索引、名字表偏移基准完全相同 |
| `bin_sjis_to_utf8` | `bytes.decode('cp932','replace')` | CP932（`sjis` 别名）；非法序列→U+FFFD，消费 1 字节 |
| `bin_find` | 名字→序号查表 | 线性扫描，1112/18612 条量级足够 |
| `fvp_inflate` | `zlib.decompress(payload)` | `graph.bin` 1112/1112 逐字节相同 |
| `hzc1_parse` | `hzc1.Hzc1Image.__init__` / `raw` 校验 | 同样校验魔数、`usize == w*h*bpp*frames` |
| `hzc1_decode_frame` / `decode_all` | `Hzc1Image.array()` + 逐帧切片 | 输出统一扩成 RGBA8888：fmt0 补 A=255；fmt3/4 复制到 RGB 且 A=255；fmt4 的 `*255` 按 numpy uint8 回绕 |
| `c_assets ppm` | `Image.fromarray(...).save()` | 只丢 alpha，RGB 字节不变 |

与 `work/hzc1.py` 的唯一差别是输出格式：Python 输出 PIL 的 RGB/RGBA/L 三种 mode，
C 端统一 RGBA8888（引擎内部格式），比对时在 Python 侧补齐 alpha/灰度扩展。

## 7. 验收（`verify_assets.ps1`）

运行：`powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1`
（`-Full` 用全部 1112 个文件，约 2 分钟；默认抽样约 25 秒）。

### 7.1 BIN

```
PASS graph.bin: 1,112 entries, names/sizes/offsets match Python (0 bad), 15 entries dumped byte-exact
PASS bin_find: all 1,112 names resolve to their index, unknown name -> -1
PASS non-ASCII name decodes to ['x禁止'] (CP932 x禁止)
PASS names match the 1,112 bin_archiver_extracted files
PASS other packages: 11 packages / 20,613 entries match Python
PASS synthetic package: 10 CP932 edge-case names + empty/odd payloads match Python
```

* 12 个包共 21725 条，索引/名字/大小/偏移全等；
* `dump` 随机 + 首尾共 15 条，导出字节与包内切片逐字节相同（读数据路径）；
* 合成包覆盖 CP932 边界：`\x81\x20`（非法尾字节）、`\x80`、`\xA0`、`\xFD\xFE\xFF`、
  `\x81`（孤立前导字节）、`\x82\xA0`（正常双字节）、Tab 名字、空 payload。

### 7.2 inflate

```
PASS 1112 all hzc1 payloads: 2,539,861,820 bytes decompressed byte-identically to Python zlib.decompress
PASS 84 synthetic streams (42 zlib + 42 raw, levels 0-9, 0 B .. 70000 B) round-trip exactly
PASS fuzz: 100,000 random/mutated/truncated streams, no crash, no guard-byte violation
         (ok=11328 err=88672 guard_violations=0)
PASS fuzz (second seed): 25,000
PASS 25 truncated/bit-flipped real streams: error or clean success, never a crash
```

* 默认抽样 200 个文件（455,298,844 字节），`-Full` 为全部 1112 个（2.54 GB），均逐字节相同；
* 合成流覆盖 zlib/裸 deflate × level 0/1/2/5/6/9 × 空/1B/3B/全 256 字节/重复串/随机 70 KB；
* fuzz 在输入/输出缓冲区两端各放 64 字节哨兵（0xA5/0x5A），每轮检查未被写坏；
  输入含纯随机、真实流变异（1–12 个 bit 翻转）、追加垃圾、随机截断，输出缓冲 0–1 MiB；
* 截断/翻转真实流时返回错误码或干净成功，没有一次崩溃。

复现随机样本的种子固定为 `--seed 20261123`（默认）。

### 7.3 HZC1

```
PASS scanned 1,112 hzc1 files
PASS header metadata matches Python for all 1112 sampled files
PASS 1112 images / 3,654 frames / 727,573,943 pixels: 0 differing pixels
     (fmt0=12 fmt1=929 fmt2=104 fmt3=55 fmt4=12)
PASS per-frame API: 3 multi-frame files, frames 0/mid/last byte-exact
PASS PPM export: 5 formats (fmt 0,1,2,3,4) valid P6 with exact RGB pixels
```

* 默认抽样：每个 fmt 至少 5 张 + 随机补足 100 张（固定种子）；
  `-Full` 为全部 1112 张（3654 帧、7.28 亿像素），**0 像素差异**；
* 比对方式：C 端 `rgba … all` 写出 `frames*w*h*4` 字节，Python 端用
  `work/hzc1.py` 的 `array()` 生成同布局 RGBA，再逐像素（4 字节）比较；
* 多帧文件额外用 `rgba <file> <out> <index>` 校验第 0/中间/最后帧的切分；
* PPM 导出校验 `P6\nW H\n255\n` 头与像素体（丢 alpha 后与参考 RGB 相同）。

## 8. 性能

环境：w64devkit gcc 16.2.0，`-std=c99 -O2`，Windows x64；`clock()` 计时（CPU 时间），
每次 bench 跑 10 轮取平均；数据在文件缓存中，属于稳态 CPU 耗时。

| 样本 | 格式 | 尺寸 | 压缩后 / 原始 | 解码耗时 | 吞吐（原始像素） |
|---|---|---|---:|---:|---:|
| `0291_bl_mask` | fmt1 BGRA32 | 1920×1080 | 21,496 B / 8,294,400 B | **10.6–12.2 ms** | 650–750 MiB/s |
| `0220_a_cg_s1` | fmt0 BGR24（真实 CG） | 1920×1080 | 2,194,642 B / 6,220,800 B | **27.0–27.9 ms** | 210–220 MiB/s |
| `0494_diss00` | fmt3 Gray8 | 1920×1080 | 3,424 B / 2,073,600 B | **5.4–5.7 ms** | 347–366 MiB/s |
| `0001_000101W_F` | fmt2 BGRA32 ×4 帧 | 135×112 | 76,886 B / 241,920 B | 0.175 ms/帧 | 330 MiB/s |

拆分（`bench-inflate`，只测 inflate、不含转 RGBA）：

```
0291_bl_mask: 21,496 B -> 8,294,400 B, 7.35–8.0 ms/次 ≈ 990–1076 MiB/s（约 1 GiB/s）
```

结论：

* **一张 1920×1080 图约 10–30 ms**，取决于压缩流复杂度（`0291_bl_mask` 是近乎纯色的
  遮罩，压缩率高、inflate 快；`0220_a_cg_s1` 是真实 CG，2.2 MB 压缩流，慢一些）。
  折算 30–95 FPS，单帧播放足够，离「全库 516 MB 秒级解包」的目标也不远；
* inflate 本身约 1 GiB/s（高度可压缩数据），转 RGBA 是纯内存拷贝 + 通道交换，
  1920×1080 约 1–3 ms；
* 多帧文件（fmt2）建议用 `hzc1_decode_all` 一次 inflate 解全部帧，避免每帧重复解压。

如果以后要提速：给 `hzc1` 加「解码句柄」缓存 raw 块（现在的 API 每帧都重新 inflate），
或为 inflate 增加更长的根表 / 4 字节快表；当前实现优先简单与可验证性。

## 9. 已知限制 / 未覆盖

**inflate**

* 单条流、一次性解压，不做流式/增量接口；输出必须放得下（超 `dstlen` 报错，不截断）；
* 不支持 gzip 包装（`1F 8B`）、不支持需要预置字典的 zlib 流（FDICT → `ERR_HEADER`）；
* 自动识别对裸 deflate 有理论歧义（见第 4 节），需要时用 `fvp_inflate_raw()`；
* 输出长度以 `int` 返回，单次 >2 GiB 不支持（资源包里最大 17 MB）；
* 错误码只区分大类，不与 zlib 的 `Z_DATA_ERROR`/`Z_BUF_ERROR` 一一对应。

**HZC1**

* 只认 `hzc1`+`NVSG` 魔数；旧版 `hzc`（无 `hzc1` 头）不支持；
* 输出固定 RGBA8888，不做屏幕坐标裁剪/缩放（`x/y` 仅作为元数据保留）；
* `hzc1_decode_frame` 每次重新 inflate 整条流（无跨调用缓存），批量解码请用 `hzc1_decode_all`；
* fmt=4 的 `*255` 按 numpy uint8 回绕语义实现（实际数据只有 0/1，等价于 0/255）；
* 逐帧时长不在文件里（见 `hzc1-format.md` §6.3），本层不处理时间轴；
* 头部 0x08/0x1C/0x24/0x28 字段只读取不校验（`0x08` 恒为 32）。

**BIN**

* `bin_open` 一次性把索引和名字表读进内存：`voice.bin` 18612 条约 0.5 MB，
  若将来出现几百万条的包需改成 mmap 或分块；
* 打开时做全量入口校验（`data_off+size <= 文件大小`），对合法包无额外 I/O；
* 名字按 CP932 解码（不是严格 `sjis` codec）；本作 1112 条名字两者一致；
* 错误信息为全局静态缓冲，非线程安全；多个 `BinArchive` 本身可并存；
* 文本工具协议（`list`/`findall`）以换行分隔条目，名字里若含换行无法表示
  （本作数据没有这种名字）。

**验收工具**

* `c_assets` 的 `inflate`/`rgba` 等命令用 `long`/`ftell` 读输入文件（<2 GB 足够）；
  BIN 包读取本身是 64 位偏移；
* fuzz 是「随机 + 真实流变异」，不是穷举；已覆盖 stored/fixed/dynamic 三种块，
  但极长距离/极长输出（接近 2 GiB）的路径没有压力测试；
* 验收依赖游戏数据（`graph.bin` 与 `extract/graph`）与 Python 参考实现；
  缺少数据时脚本会 FAIL 并说明缺哪个路径，不会静默跳过。

## 10. 接进引擎

三个模块互相独立、只依赖 libc：

```
src/bin.c      -> #include "bin.h"      只依赖 libc + gen_cp932.h
src/inflate.c  -> #include "inflate.h"  只依赖 libc
src/hzc1.c     -> #include "hzc1.h"    依赖 inflate.h
```

接到 `fvm.exe` 时把 `src/bin.c src/inflate.c src/hzc1.c` 加进 `Makefile` 的 `SRC`
（`bin.c` 需要 `-I src`，与现有编译命令一致），即可在 `main.c`/`syscall.c` 里
用 `bin_open`/`hzc1_parse`/`hzc1_decode_frame` 实现 `GraphLoad` 之类命令。

已做链接验证：把这三个 `.c` 与现有 `src/fvp.c encoding.c hcb.c vm.c syscall.c main.c`
一起用 `-std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv` 编译链接，**零警告、无重复符号**：

```powershell
gcc -std=c99 -O2 -Wall -Wextra -Wpedantic -fwrapv -Ifvp\src -o fvp\out\probe.exe `
    fvp\src\fvp.c fvp\src\encoding.c fvp\src\hcb.c fvp\src\vm.c `
    fvp\src\syscall.c fvp\src\main.c `
    fvp\src\bin.c fvp\src\inflate.c fvp\src\hzc1.c
```

本次改动**没有**触碰 `vm.c`、`hcb.c`、`syscall.c`、`encoding.c`、`main.c`、`Makefile`、
`build.ps1`、`work/`、`extract/`、`gen_*.h`。

## 11. 复现

```powershell
# 默认抽样验收（约 25 s）
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1

# 全量验收（1112 张图 / 1112 条 zlib 流，约 2 min）
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1 -Full

# 小样本冒烟
powershell -ExecutionPolicy Bypass -File fvp\tests\verify_assets.ps1 -Quick

# 只跑工具
fvp\bin\c_assets.exe list "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\graph.bin"
fvp\bin\c_assets.exe hdr  fvp\extract\graph\0291_bl_mask
fvp\bin\c_assets.exe ppm  fvp\extract\graph\0220_a_cg_s1 fvp\out\a_cg_s1.ppm
fvp\bin\c_assets.exe dump "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\graph.bin" 0 fvp\out\entry0.hzc1
fvp\bin\c_assets.exe fuzz 100000 20261123
```

验收脚本每次运行会重建 `fvp/out/verify/`（临时文件，可随时删除）。