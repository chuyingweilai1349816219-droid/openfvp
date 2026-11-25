# 项目现状与差距总表（第 120 轮收尾）

> 本项目由 **6amm** 辅助 DeepSeek 完成，其中所有代码由 DeepSeek 生成。
> 赞助：<https://afdian.com/a/6ammm>
>
> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

这一页不是"功能介绍"，是**收尾时的对账**：现在到底到哪一步、每个数字怎么复核、
哪些是"没做"而不是"做坏了"。所有数字都能用本文末尾的命令重跑出来。

---

## 1. 一段话说清现状

引擎本体已经能**跑通两部目标作品**（`iroseka.hcb` / `Hoshimemo_HD.hcb`）：
脚本解释器与 Python 参考实现**逐字节一致**，渲染层与原版真值画面**平均差 5.4%**，
桌面外壳能开窗、能跑 60 fps、能存能读文件。没做到的是**"点进去像原版一样把游戏玩完"**：
读档只能"部分恢复"（原版存档里没有 pc/栈）、影片跳、设置项大多只读、
引擎侧 UI 只做了标题菜单与一个游戏内菜单。

| 维度 | 估计 | 依据 |
|---|---:|---|
| 引擎 / 运行时完整性 | **≈ 85–90%** | 148 条命令、40 个 opcode、资源/音频/存档/渲染全通；缺 WMV、LipSync、少数 UI 子系统 |
| 内容可玩度（能正常推进多少） | **≈ 55–65%** | 序章 + 正文可观看到逐字节级别；分支/存档恢复/菜单流程不完整 |
| "逐像素等于原版" | **部分** | 序章文本逐字节；标题画面实拍 mean_abs **13.79/255**（≈5.4%）；UI 字体协议已挖到并实现成开关 |

---

## 2. 七项目标的对账

| # | 目标 | 状态 | 证据 / 门禁 | 还差什么 |
|---|---|---|---|---|
| ① | 第二部作品兼容 | ✅ | `verify_multigame.py`：两作命令表 148 条逐条相同、VM 零修改跑出 164 行台词、桌面各出帧 | 第三部以后的作品没测过 |
| ② | 交互闭环 | 🟡 | 标题菜单 5 项（hover/点击/存档列表/白场过渡）、游戏内菜单 SAVE/LOAD/OPTION、`SaveWrite` 写真实槽 | `Load(40)` 只能"部分恢复"——存档里没有 pc/栈（`docs/save-snapshot.md` §4.6）；CG/回放/历史回溯界面没做 |
| ③ | 文本呈现 | 🟡 | `TextFont`(113) 属性、字体表模型、逐码点缺字回退、**原版 GDI 协议开关** | `TextShadowDist`/`Format`/`Function`/`Space` 仍是固定版式；`TextFormat` 的 d 参数（行首缩进）会动冻结基线，**需要授权才改** |
| ④ | 动画缓动 mode 1..5 | ✅ | `test_motion` 85 项：5 个 mode 的端点/中点/单调性/形态特征值 | 与实机动画曲线没有逐帧对照 |
| ⑤ | 语音 | 🟡 | 声道模型（BGM/BGM_ALT/VOICE/SE）、600 ms 交叉淡入、三级音量链 + 51 + 24 项单测 | LipSync 未在真实运行里观察到；语音 id 依赖存档层 |
| ⑥ | WMV 视频 | 🔴 | 只有 `Movie*` 跳过桩，画面不播 | 需要解码器（大项，独立于其它部分） |
| ⑦ | 跨平台 OS 抽象 | ✅ | `fvp_plat_*` seam + `bin/fvp-headless.exe` + `verify_headless.py` 门禁 | Android 外壳只有设计文档 |

---

## 3. 六个缺口的最新状态

> 这六条是 2026-11-25 中期列出的"离原版一模一样还差什么"。第 118–119 轮之后
> **⑥ 被降级**了：其中一半不再需要跑原版。

| 缺口 | 原判断 | 现状态 | 依据 |
|---|---|---|---|
| ① 文字管线（图集 vs GDI） | 需要原版对照 | 🟡 **已实现原版协议** | `ref/exe-gdi-params.json` 提取 + `tests/test_gdi_ggo.c` 28 项；`--glyph-original` 切换 |
| ② 缺原版帧语料 | 必须实拍 | 🔴 仍缺（等用户的原版日文版） | `docs/original-verification.md` §7 语料在 `out/`（不入库） |
| ③ 特效/叠加层参数 | 未知 | 🟡 部分已落地（背景叠加、菜单配色） | `docs/title-menu.md` §4.9 |
| ④ 存档无 pc/栈 → 不能续玩 | 硬限制 | 🔴 不变（可做"重放到台词"的近似） | `docs/save-snapshot.md` §4.6 |
| ⑤ WMV 影片 | 未实现 | 🔴 不变 | 同 ⑥ 目标 |
| ⑥ 原版 UI 一模一样 | "必须实拍逼近" | 🟢→🟡 **已拆成 A/B 两类**：A 类协议/常量已静态提取并冻结；B 类最终像素仍需实拍。且新线索：标题字的衬线体**不是** GDI 画的（全 exe 只有 1 个 `CreateFontA`，只能建 4 个日文非衬线族名）→ 更可能是**预渲染贴图** | `docs/original-verification.md` §8 |

---

## 4. 可复核的数字（全部能重跑）

| 指标 | 数值 |
|---|---|
| C 版 vs Python 参考（900k 步） | **6,496 字节 / 195 行**，sha256 `7f9837ffaf3b7b5759581c625698c9554e83f2cdbdeadf635dca6fc9c01b3ea2` |
| C 版 vs Python 参考（2M 步） | **15,378 字节 / 450 行**，sha256 `32d7d4e2dce4…` |
| 渲染层像素 | **293 px / 2,073,600 = 0.0141%**，PSNR 49.94 dB，SSIM 0.99971 |
| 标题画面 vs 原版实拍 | mean abs diff **13.79 / 255**（基线 13.93 + 1.5） |
| 原版绘制协议（静态提取） | `fdwCharSet = 0x80`、`GetGlyphOutlineA` 只用 `GGO_METRICS`/`GGO_BITMAP`、`MAT2` = 单位矩阵、字体名表 2 份 × 4 项 |
| 抗锯齿 vs 原版协议（同字同字号） | 「日」@32：**54 / 1224 ≈ 4.4%** 的像素是原版根本不会产生的中间值 |
| `make check` | **30 组**单测全 PASS |
| `tests/run_all.py` | **45 PASS / 0 FAIL / 1 SKIP（46 步）**，约 275 s；SKIP 是缺 numpy 的 `verify_assets` |
| `scripts/check-docs.py` | PASS：**31 篇**文档，引用路径 0 缺失，DeepSeek 声明齐全 |
| 健壮性 | 27,300 次 HCB 变异（0 崩溃）、339 个资产畸形用例、43 个 VM 边界用例 |

---

## 5. 怎么一键复核

```bash
# 构建（w64devkit 的 cc 要在 PATH 里）
mingw32-make -f Makefile all headless check

# 核心三道
python tests/compare.py --run                     # 逐字节
powershell -ExecutionPolicy Bypass -File tests/verify_gfx.ps1 -SkipDump   # 像素
python tests/run_all.py                           # 全部 46 步

# 原版相关（缺语料/缺 exe 时自己 SKIP 并返回 0）
python tests/verify_exe_gdi_params.py             # 从 exe 静态提取的绘制协议
python tests/verify_vs_original.py                # 与原版实拍帧的平均差

# 文档
python scripts/check-docs.py
```

---

## 6. 下一步（**先结构分析文件，不要先抓帧**）

> 2026-11-25 立的方法论（用户要求，已写进项目目标）：**能靠结构分析原版游戏文件解决的事实，
> 一律先结构分析，不用抓帧/视觉方法反推。** 抓帧只在方法覆盖不到的地方用（最终像素验证、
> 影片解码后的画面），而且只用来**验证**结构分析结果，不用来当主要依据。

1. **`TitleMenu` 的按钮分布（脚本参数，优先做）**：`TitleMenu` 是 FVP **脚本命令**
   （cmd 141）。证据：exe 的 61 个 MSVC RTTI 类里**没有** TitleMenu 类；HCB 内嵌命令表
   里有 `TitleMenu`；脚本里有 **6 处** `03 8D 00` 调用（`0x5a794`、`0x5a858`、`0x5f5bb`、
   `0x6133d`、`0x61367`、`0x64073`）。→ 用现成解码器把这 6 个调用点的前导 `push` 实参
   dump 出来，得到菜单项数与命中矩形；并查清 `ref/cmd_table.json` 的 `argc=1` 与 HCB
   名字表里该命令后随字节 `0x06` 的矛盾（若真是 6 参数，VM 只弹 1 个会栈失衡）。
2. **`TextShadowDist`(125) / `TextFormat`(118) 的真实参数**：先查 exe 里 `HTextStyler`
   类（这个类有 RTTI）的字段读写与命令 handler 的写入偏移，再谈实现；
   `TextFormat` 的 d 参数会移动冻结基线，需要授权。
3. **`Load(40)` 的续玩**：先确认存档尾部的协程上下文里到底有没有 pc/栈
   （`docs/save-snapshot.md` §4.6 的结论要再验一次），再决定"重放到台词"还是"真恢复"。
4. **效果/叠加层参数（③）**：查 exe 里 `HPrimAlpha` / `HPrimAnim` / `HGraphPrim`
   的字段布局与 D3D 渲染状态设置，而不是靠画面反推。
5. **WMV（⑥）**：先确定影片容器/编码（资源包里的文件头 + exe 里的 DirectShow 用法），
   再选解码器；解出来之后才谈画面。
6. 上述都做完之后的**验证环节**才用原版抓帧：标题 1 帧、正文 1 帧、游戏内菜单/设置/
   存档各 1 帧、OP 几帧（`scripts/capture-original.ps1`）。

---

## 7. 已知限制（照实写，不粉饰）

- **不提交任何游戏数据**：语料、抓帧、`.hcb`、字体图集全在 `out/`（gitignore）；
  仓库里只有参数、代码与文档。
- **"逐字节一致"只证明到序章**：900k / 2M 两档；其它步数没有参考可比。
- **原版实拍对照依赖环境**：本机 `iroseka_HD.exe` 是全屏 D3D9，`PrintWindow` 时好时坏；
  字体版本/DPI 会影响像素差。
- **`Load(40)` 的"部分恢复"是近似**，不是原版行为（原版存档不含 pc/栈，
  见 `docs/save-snapshot.md`）。
- **汉化文本超出原版能力**：原版是 cp932 的 ANSI 程序，cp932 之外的简化字它自己也
  画不出来——这不是我们的 bug（`docs/c-gfx.md` 末节）。
- **`TextFormat` 的行首缩进没动**：改了会移动冻结基线，需要明确的授权。
- **Android 外壳**只有设计文档，不是当前主线。