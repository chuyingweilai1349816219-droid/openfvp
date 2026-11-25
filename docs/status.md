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
| 命令表 argc 实测对账 | 两部作品 **825 / 865** 个可判定调用点，**82 / 83** 条命令的 argc 被证实；仅 3 条已知表达式临时值异常（两作完全相同） |
| 抗锯齿 vs 原版协议（同字同字号） | 「日」@32：**54 / 1224 ≈ 4.4%** 的像素是原版根本不会产生的中间值 |
| `make check` | **32 组**单测全 PASS（新增 `test_titlebg` 503 项） |
| `tests/run_all.py` | **74 步**（本轮加 `verify_exe_history.py` 后全 PASS；缺游戏数据时相关步 SKIP） |
| `scripts/check-docs.py` | PASS：**33 篇**文档，引用路径 0 缺失，DeepSeek 声明齐全 |
| 健壮性 | 27,300 次 HCB 变异（0 崩溃）、339 个资产畸形用例、43 个 VM 边界用例 |
| 读档续玩 step-1 | `--load-restart`：读槽文件（thumb 200×113/tail 584.9 KB）→ 场景复位（prim/motion/text）→ 重建 VM 从入口重启并保留 globals/flags；重启后脚本继续跑（40952 步、有文本）、画面非黑；槽尾快照段暂不应用 |
| 存档/读档引擎侧流程 | `Load(40)`/`SaveWrite(96)` handler、请求字段 `0xFCA/0xFCB/0xFCC`、每帧消费点、线程重启 `+0xFD9`、路径 `%s/save/s%03d.bin` 已冻结（`ref/exe-load-flow.json`）；`0x4344C0` 字段映射与场景选择仍未知 |
| 存档槽写档顺序（2026-11-26） | 序列化器 `0x43DE90..0x43E67B`（**2,028 B**）逐指令读完 → **31 段**（5 文件头 + 26 槽尾）冻结进 `ref/exe-save-serialize.json`；两种 u16 长度语义分清（槽头字符串**不含** NUL，`0x434410` 动态串**含** NUL）；6 份真实存档验证 4 条可证伪推论：32 个协程快照紧贴 tail+0（活跃恒为 0..7 号）、128 槽字符串表正好止于 `tail+0x16608`、`tail+0x18 = 02`、末两字节 `01 01` |
| 台词历史（本轮） | `HistorySet(26)` 0x43A5E0 / `HistoryGet(25)` 0x43A490 的判据、容器构造（**count 初值 = 1**，不是 0）、记录环（100 × 12B `{text0,text1,value}`）的写 0x437FA0 / 读 0x437EE0、读档回填全部冻结进 `ref/exe-history.json`；C 侧修掉"用 argc<1 当 null"的判据、`fvp_history_init` 改成 count=1，新增 VM 级用例（`test_history` 172→**202 项**）；6 份真实存档的槽尾历史环全部解析成功（s481 `count=1/pos=0`、s905 `count=11/pos=10`，最新一条台词 == 槽头台词）；**槽尾第 20 段 = 台词历史环**（旧文档误标"全局变量 + flag 位图"）；`begin` 的真实触发源仍列为未解 |
| 脚本开关（global3 / global236 / global232） | `global3` = 体验版布局开关：两作都只有 1 个写点且写 0 ⇒ 隐藏 1521/1522/1528/1529、trial 徽标、−81 px 分支都**不可达**；`global236`（五彩）/`global232`（星空）= 跳过动画开关（写 0/1 都有，读点 25/24）。`ref/script-globals.json` + 门禁 `verify_script_globals.py` |
| RS 分支变换 / 旋转角单位 | exe 0x42B21E：`final = T2 + S·(R·(图头−OP) + prim + accum)`，`T2 = 画面中心`；**渲染 1 角度单位 = 0.05°**（`a/3600·π`，7200 一圈；旧文档的 0.1° 已修正）。`ref/exe-rs-transform.json` + 门禁 `verify_exe_rs_transform.py` |
| 截图门禁确定性 | 新增 `--title-no-hover`：之前"静态标题帧"会随宿主鼠标停在菜单上变成 hover 变体，`verify_opening_render` 偶发 0.42 mean_abs 失败；三个渲染门禁现在都带这个开关 |
| 标题背景动画（两作都已接画面） | 星空 2000→1000/40000 ms（**40 s 后与静态帧逐字节 0 差**、~55 fps）；五彩四层 path A/B 缩放+旋转/25000 ms（动画中 vs 静态 mean_abs 20.96、1/4 分辨率合成 ~52 fps、相邻帧差 min 0.178/median 0.386/max 1.13） |
| 标题背景缩放/旋转（参数基线） | 五彩 4 层 258..261：缩放 1300/1600/2500/4000→900/800/800/800、旋转 15/25/55/125°→0°（25000 ms、mode 3），OP=(1200,675)；星空只有 258：2000→1000、40000 ms、**不旋转**。HZC1 头无旋转信息 |
| 标题进场（脚本时间轴） | 逐元素错开，**两作各一套**：五彩 logo 2000/菜单 2400（总 4000 ms）、星空 logo 1000/菜单 1400（总 3000 ms）；端到端：五彩 mean_abs 5.02、星空 3.31，两作进场结束后都与静态帧逐字节 0 差 |
| 开场 FAVORITE logo（脚本时间轴） | **3800 ms**（1200 淡入 + 2500 淡出 + 100 收尾，旧拟合值 5400 已作废）；六层/逐层 alpha/缩放/旋转/位移；**两作**端到端：五彩 淡出中 mean_abs 47.97、星空 32.34，两作结束帧都与静态标题帧**逐字节 0 差** |

---

## 5. 怎么一键复核

```bash
# 构建（w64devkit 的 cc 要在 PATH 里）
mingw32-make -f Makefile all headless check

# 核心三道
python tests/compare.py --run                     # 逐字节
powershell -ExecutionPolicy Bypass -File tests/verify_gfx.ps1 -SkipDump   # 像素
python tests/run_all.py                           # 全部 74 步

# 原版相关（缺语料/缺 exe 时自己 SKIP 并返回 0）
python tests/verify_exe_gdi_params.py             # 从 exe 静态提取的绘制协议
python tests/verify_script_argc.py                # 命令表 argc 与真实调用点对账
python tests/verify_vs_original.py                # 与原版实拍帧的平均差

# 文档
python scripts/check-docs.py
```

---

## 6. 下一步（**先结构分析文件，不要先抓帧**）

> 2026-11-25 立的方法论（用户两次追加，已写进项目目标）：
> **遇到问题先考虑结构分析。抓帧最多只能做到"看起来一样"，做不到"实际操作上一样"**——
> 它拿不到参数、单位、取值范围、状态机、缺省语义、取消/回滚行为。
> 抓帧只剩两个合法用途：① 最终像素验证（B 类，受字体版本/GPU 混合/DPI 影响）；
> ② 影片解码后的画面。而且**结构分析结论与截图扫描冲突时，以结构分析为准**，
> 再去查截图为什么不同（相位/时间/环境差异）——第 130/131 轮的 `MotionMoveS2`
> 之争就是范例：截图说 1.0、脚本参数被我误读成 0.9，最后是 handler 静态分析
> （时长 7 ms + Stop 只取消）把结论定成 1.0。

0. **开场/标题的动画收尾（见 `docs/opening.md` §6）**：① 标题背景的 25 s 慢缩放 + 慢旋转（`MotionMoveS2 1300→900`、`MotionMoveR 150/250/550/1250→0`，均 25000 ms）需要把标题背景改成按图元 OP 轴心绘制；② 标题装饰层 262/263/264 （`title_frame`/`title_frame2` 找不到 GraphLoad 调用点，见 §21 与 `ref/title-enter.json`）；③ 1522/1529 的 `global3` 分支特例（终点 −81 px）；④ 开场函数里的 `call 0x3F6A0` / `call 0x42AC6` 两个 helper（疑似 BGM/淡入）。
1. **把标题菜单的图元矩形按素材头改正（第 123 轮已解出，见 `docs/title-menu.md` §9）**：
   `ref/title-rects.json` 里是**原值**——五彩斑斓右边缘全部 **1785**、行距 **100**、
   行 y=495/595/695/795/895，`title_option` 素材高 **90**；星空居中 **960**、行距 **90**。
   下一步：把 `src/titlemenu.c` 的 profile 常量（现在是截图反推的 `img_y0=492`）改成素材头
   的值，并单独量标题画面的像素差（会动标题渲染，需与 `verify_vs_original` 一起看）。
   历史记录（已完成）：
   标题画面是**脚本画的**——`title_*` 素材 + `GraphLoad` + `PrimHit`；菜单项就是图元
   （五彩斑斓 **1501/1508/1515/1522/1529**，星空 **266/268/270/272/274**）。
   下一步把这些图元的 `PrimSetSprt`/`PrimSetXY`/`PrimSetWH`/`PrimSetUV` 实参解出来，
   得到与原版一致的按钮矩形，冻结成基线并用 `PrimHit` 语义做门禁。
   工具与基线：`scripts/extract-title-scene.py`、`ref/title-scene.json`、
   `tests/verify_title_scene.py`。**全程不需要抓帧。**
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