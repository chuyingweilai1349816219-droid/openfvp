# 文档索引

这里的文档是**结构分析与实现过程中的工作记录**：格式怎么来、结论怎么验证、哪里还没做对。
写作原则是「只写验证过的，猜想单独标注」（各文档内部都会明确区分）。

一共 14 份专题文档 + 本索引。建议阅读顺序：先 [00-engine.md](00-engine.md) 建立全局印象，
再按你关心的方向跳到对应分组。

> 关于路径：文档里引用的 `work/`、`out/`、`extract/` 是**本地工作目录，不进仓库**
> （见根目录 [`.gitignore`](../.gitignore)）。这些引用描述的是当时的验证手段，
> 结论本身已写在文档里；想核对时用仓库内同一目录下的 Python 脚本
> （如 [`tests/verify_assets.py`](../tests/verify_assets.py)）或 C 工具重跑。
> 所有**仓库内**的文件路径由 [`scripts/check-docs.py`](../scripts/check-docs.py) 定期校验。

---

## 格式（容器 / 脚本 / 图像 / 文本）

| 文档 | 讲什么 |
|---|---|
| [00-engine.md](00-engine.md) | 阶段 0 总报告：主程序结构、BIN / HCB 容器格式、命令表内嵌方式、VM 规模，是其余文档的起点 |
| [01-assets.md](01-assets.md) | 资源包清单：12 个 `.bin` 包的条目数、解压后大小、包内实际格式（图像 / 音频 / 脚本） |
| [02-text-format.md](02-text-format.md) | `.hcb` 字符串与内联注音 `[汉字\|假名]`、强调点标记的统计与解析规则 |
| [hzc1-format.md](hzc1-format.md) | HZC1 / NVSG 图像格式完整结构分析：44 字节头逐字段、fmt 0–4、多帧纵向拼接、屏幕坐标字段 |

## 虚拟机（架构 / 解释器 / 命令语义）

| 文档 | 讲什么 |
|---|---|
| [03-architecture.md](03-architecture.md) | 模块划分与分层（platform / gfx / snd / syscall / vm），以及各层的职责边界 |
| [c-vm.md](c-vm.md) | C 版 VM 重写：验收表（900k / 2M / 20M 步逐字节一致）、代码结构、设计说明、与 Python 的保真清单 |
| [vm-commands.md](vm-commands.md) | 从静态分析得出的 syscall 分发机制，以及 148 条命令的 id / 名字 / argc / 已知语义 |
| [05-threads.md](05-threads.md) | 线程与时间模型研究笔记：已确认的线程槽布局与 `Thread*` / `Timer*` 命令语义、`initstack` 第二个操作数的发现，以及尚未实现调度/时间模型的失败记录 |
| [cn-script-rebuild.md](cn-script-rebuild.md) | 被汉化补丁打坏的 GBK `.hcb` 如何以日文版为骨架重建，含字节账、重定位与校验方法 |

## 渲染

| 文档 | 讲什么 |
|---|---|
| [render-model.md](render-model.md) | 渲染层模型（Python 原型）：图元 / 图层 / 文本排版 / 合成顺序，以及第 7 节的**已知偏差与未实现语义** |

## 音频

| 文档 | 讲什么 |
|---|---|
| [04-audio.md](04-audio.md) | 素材格式（Ogg Vorbis vs RIFF/WAVE）、采样率不一致的处理，与 [`src/snd.c`](../src/snd.c) 的解码 / 混音接口 |

## 工具链

| 文档 | 讲什么 |
|---|---|
| [c-assets.md](c-assets.md) | C 资源层（BIN / inflate / HZC1）的实现、API、限制，以及逐字节 / 逐像素的验收方法 |

## 移植

| 文档 | 讲什么 |
|---|---|
| [android-shell.md](android-shell.md) | Android 外壳与构建流水线（进行中，非当前主线） |
| [roadmap.md](roadmap.md) | 阶段划分、阶段 0 的结论、关键技术风险与版本目标 |

---

## 相关但不在本目录的文档

| 文件 | 讲什么 |
|---|---|
| [README.md](../README.md) | 项目总览、现在能做什么 / 不能做什么、构建与运行 |
| [CONTRIBUTING.md](../CONTRIBUTING.md) | 如何报兼容问题、如何做等价性验证、代码风格、提交前检查 |
| [THIRD-PARTY.md](../THIRD-PARTY.md) | 第三方代码（`stb_vorbis`）与生成文件的来源和许可 |
| [ref/REFERENCE.md](../ref/REFERENCE.md) | 冻结的 Python 基准与各期望输出的 SHA256，是 C 侧验收的锚点 |