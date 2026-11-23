# 跨作品验证：两部 FVP 游戏

> 以下内容由 DeepSeek 生成,不一定代表作者个人意愿

用一个引擎跑两个游戏，才叫「引擎」；只能跑一个游戏，那是为该游戏定制的工具。
本文记录用《星空のメモリア HD》对《いろとりどりのセカイ HD》的验证结果。

## 两部作品

| | A | B |
|---|---|---|
| 作品 | いろとりどりのセカイ HD Re:GENERATION | 星空のメモリア -Wish upon a shooting star- HD Edition |
| 原版年份 | 2011（HD 2021） | 2009（HD 2019） |
| 主脚本 | `iroseka.hcb` 4,275,282 B | `Hoshimemo_HD.hcb` 4,618,610 B |
| 目录体积 | 9.36 GB | 8.89 GB |

## 结果

### 资源包：命名完全一致

两部作品都是同一组包名：

```
graph.bin  graph_bg.bin  graph_bs.bin  graph_vis.bin  graph_vis1.bin  graph_vis2.bin
bgm.bin    bgm2.bin      voice.bin     se.bin         se_env.bin      se_sys.bin
movie/01.wmv
```

条目命名规律也一致：

| 前缀 | 用途 | 例 |
|---|---|---|
| `BG*` | 背景 | `BG001_000` |
| `CHR_*` | 立绘 | `CHR_かーくん_基_乗り`、`CHR_あゆむ_基_ねこ_吹出C` |
| `*_e01a` / `*_h01a` | 角色 CG | `ASUHO_e01a`、`ETC_e01a` |
| `a_bu_*` / `bu_*` | 特效 | `a_bu_cp1`、`bu_effect15_L_1` |

### 脚本：解析器零修改通过

```
Hoshimemo_HD.hcb  4,618,610 字节
  指令      1,135,159
  字符串       60,121
  syscall       4,983
  命令种类        131
  解析          一路到底，无中断
```

对照 iroseka：948,219 指令 / 55,461 字符串 / 4,983 syscall / 131 命令种类。

### 命令表：148 条，逐条完全相同

这是最关键的一条。两张表按 **id 顺序**逐个比对（名字 + argc），**差异 0 条**：

```
iroseka   148 条
hoshimemo 148 条
不一致条目：0
```

说明 FVP 的引擎 API 在 2009 → 2021 这十二年间**没有增删改**。一个实现覆盖两部作品。

### opcode：同一套 40 个

两部作品用到的 opcode 集合完全相同（`garr_read/garr_write/larr_read/larr_write` 这些少见指令也都出现），只是频率不同：

| opcode | iroseka | hoshimemo |
|---|---:|---:|
| call | 184,798 | 211,933 |
| jmpcond | 8,141 | 9,357 |
| initstack | 3,435 | 4,982 |
| garr_read | 1,507 | 1,441 |
| larr_write | 79 | 30 |

### 虚拟机：直接跑，输出正确台词

`fvm.exe` **未做任何修改**，直接跑 B 的脚本 90 万步，输出 164 行：

```
[6] 　　丘の上　　
[0] ~　俺は彼女が好きだった。
[0] 　それは恋とは違う好きだった。
[0] 　今となってはその想い出は、曇ったガラス越しに見ているみたいに
     ところどころが薄ぼんやりとしているのだけれど。
[0] 　証拠を挙げるのなら、俺たちはまだ幼かったのに~ファーストキスまで交わしていた。
```

## 观察到的差异（不影响兼容性）

1. **文本层号不同**：iroseka 用层 0 正文 / 8 人名 / 9 章节标题；hoshimemo 的地点标题在**层 6**。层号是脚本自己选的，引擎只按层号处理，所以不需要改代码——但渲染层不能把「层 9 = 标题」写死。
2. **`~` 出现在文本里**：hoshimemo 的换行/停顿用 `~` 表示（iroseka 没见过）。这是文本层的排版记号，需要在文本渲染里处理。
3. 指令密度不同（B 比 A 多 20%），属于剧本长度差异。

## 结论

- 容器格式（BIN）✅ 通用
- 图像格式（HZC1）✅ 同一套（待用 B 的资源跑一次确认，见下）
- 脚本格式（HCB）✅ 通用
- 虚拟机语义 ✅ 通用
- 命令表 ✅ **完全一致**
- 渲染层 ⬜ 待验证（依赖 syscall 接线完成）

## 待补

- [x] 用 `c_assets.exe` 解 B 的 `graph.bin`，确认 HZC1 格式一致
      —— 条目 0：`fmt=1 BGRA32 1800×880 单帧`，C 解码器直接出图（`out/c/hoshimemo-e0.ppm`）
- [ ] 对 B 的脚本做与 A 相同的结构校验（198,337 处跳转 100% 落地那种）
- [ ] 渲染层接完 syscall 后，用 B 出一帧画面

复现：

```powershell
python fvp\tools\fvpprobe.py "C:\Users\Mechrevo\Downloads\hoshimemo_installed"
fvp\bin\fvm.exe "C:\Users\Mechrevo\Downloads\hoshimemo_installed\Hoshimemo_HD.hcb" --steps 900000 --click 600 --quiet
```