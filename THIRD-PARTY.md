# 第三方代码与来源 / Third-Party Code and Provenance

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

本文件说明本仓库里**实际包含**的第三方代码，以及**被文档引用但未入库**的外部工具。
仓库内除下面第 1 节的 `stb_vorbis` 外，没有任何第三方源码：`src/` 的其余文件、
`tests/` 的全部脚本都是为本项目写的；`inflate`、编码表解码、BIN/HZC1 解析都是自己实现的
（因此构建不依赖 zlib / iconv / libvorbis）。

---

## 1. 仓库内唯一的第三方代码：`stb_vorbis.c`

| 项 | 内容 |
|---|---|
| 文件 | [`src/third_party/stb_vorbis.c`](src/third_party/stb_vorbis.c) |
| 项目 | stb_vorbis **v1.25**（单文件 Ogg Vorbis 解码器） |
| 作者 | Sean Barrett（2007），后续由多位贡献者维护 |
| 上游 | <https://github.com/nothings/stb> · <http://nothings.org/stb_vorbis/> |
| 许可 | **MIT 或 Public Domain（Unlicense）二选一**，正文见该文件末尾的 LICENSE 段 |
| 大小 / 校验 | 5,720 行 / `SHA256 97A8FEBC738E6A29FAFC8D8BC557C7F17AD3DB7BA6544485F59395CC6008B764` |
| 用途 | [`src/snd.c`](src/snd.c) 直接 `#include` 它，把 Ogg Vorbis 解成 PCM16；`make lib`、`make test` 会编译到 |
| 修改 | 上游文件原样引入，未做本地修改 |
| 许可兼容性 | MIT / Public Domain 均可并入 GPL-2.0-only 项目，不改变本项目整体的 GPL-2.0-only |

---

## 2. 生成文件（不是第三方代码，但来源要说清）

这三个头文件是**生成物**，已入库是为了让构建只需要一个 C99 编译器：

| 文件 | 生成者 | 数据来源 |
|---|---|---|
| [`src/gen_cp932.h`](src/gen_cp932.h) | [`tests/gen_tables.py`](tests/gen_tables.py) | Python 标准库 `codecs` 的 cp932 映射表（纯映射数据，不含 Python 源码） |
| [`src/gen_gbk.h`](src/gen_gbk.h) | [`tests/gen_tables.py`](tests/gen_tables.py) | Python 标准库 `codecs` 的 gbk 映射表（同上） |
| [`src/gen_cmd_table.h`](src/gen_cmd_table.h) | [`tests/gen_tables.py`](tests/gen_tables.py) | [`ref/cmd_table.json`](ref/cmd_table.json) + [`ref/cmd_argc.json`](ref/cmd_argc.json) |

`ref/*.json` 的内容是「命令 id / 命令名 / 参数个数 / 是否返回值」这类**事实性元数据**，
从 `.hcb` 脚本的附加数据段与可执行文件的命令注册表读出，不含任何游戏代码、图像或台词内容。

生成是确定性的，CI 会重新生成并与入库版本比对（见
[`.github/workflows/ci.yml`](.github/workflows/ci.yml)）：

```bash
python tests/gen_tables.py
git diff --exit-code src/gen_cp932.h src/gen_gbk.h src/gen_cmd_table.h
```

---

## 3. 被文档引用、但**不在本仓库**的外部工具

结构分析阶段用过下面这些现成工具做交叉核对。它们的源码/二进制**没有**被打包进本仓库
（`tools-src/` 在 [`.gitignore`](.gitignore) 里），这里列出仅为出处与许可的透明度。
如果你自己要用，请遵守各自许可：

| 工具 | 本地目录 | 用途 | 许可（以文件内声明为准） |
|---|---|---|---|
| `akerou/fvp`（`hcbdecoder` / `hcbrebuilder`） | `tools-src/akerou-x/fvp-master/` | HCB 脚本的解包与重打包，用于交叉核对指令边界 | 上游副本未附许可文件，未核实 |
| `vn-tools`（`bin_archiver` / `hcb_compiler` / `nvsg_converter`） | `tools-src/vn-tools-x/fvp-tools-master/` | BIN 包解包、HCB 编译、NVSG 转换 | GPL-2.0（目录内有 `LICENSE`） |
| `lodepng` | 前两个工具内部 | 只在第三方工具内部用于 PNG 输出 | zlib |

> `tools-src/` 里的第三方代码**不是**本项目的组成部分，也不在构建路径上。
> 这些副本来自社区公开发布，本仓库没有记录其上游仓库 URL，因此不在此处给出链接，
> 以免给出错误地址；需要时请按项目名自行检索上游。
>
> 另外 [`docs/hzc1-format.md`](docs/hzc1-format.md) 引用过 GARbro 的一个提交作为
> HZC1 识别的旁证（上游 <https://github.com/morkt/GARbro>），本项目未使用其代码。

---

## 4. 文档里的 Python 原型

结构分析与验证阶段用的 Python 原型（`work/fvm.py`、`work/hzc1.py`、`work/render.py` 等）
是本项目作者原创代码，**未入库**（原因见 [`docs/README.md`](docs/README.md) 与
[`.gitignore`](.gitignore)），其结论已经整理成 [`docs/`](docs/) 下的文档。
C 侧的验收基准是冻结的 [`ref/fvm_ref.py`](ref/fvm_ref.py)，它**在**仓库内。

---

## 5. 游戏数据

本仓库**不包含任何游戏数据**（`.bin` / `.hcb` / `.ogg` / `.wav` / 图像 / 台词均无）。
文档里出现的文件大小、条目数、像素数等统计值只是测量结果，不是数据本体。