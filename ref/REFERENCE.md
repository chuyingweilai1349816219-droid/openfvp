# 参考实现（冻结）

这里是 C 引擎的**对照基准**。`fvm_ref.py` 是 Python 虚拟机原型的冻结副本，改动它就会让 C 侧的等价性验收失效。

## 文件

| 文件 | SHA256 |
|---|---|
| `fvm_ref.py` | `1446D9D7DDAE548A68C27354E697C81568B656DB221EB517499ADDA0DF35FD00` |
| `cmd_table.json` | `88F5E2E4EBB09C914A8002540E53664112BF5670CC0E7F49665359E4742D0A36` |
| `cmd_argc.json` | `0ECAD110CBF5360E10514C30440C90F666B2690D349302A778EF87AB7489D1CD` |
| `ref-900k.txt` | `7F9837FFAF3B7B5759581C625698C9554E83F2CDBDEADF635DCA6FC9C01B3EA2`（195 行） |
| `ref-2m.txt` | `32D7D4E2DCE441738A79828D57EC6A7522CB639204141B5571BB42BF9BB3856E`（450 行） |

## 基准

验收以 **`ref-900k.txt`** 为准：对 `iroseka.hcb` 跑 900,000 步、`--click 600`、`--quiet`，输出 195 行。

`ref-2m.txt`（450 行）用于更长的稳定性检查。

## 复现

```powershell
python fvp\ref\fvm_ref.py "C:\Users\Mechrevo\Downloads\五彩斑斓的世界HD\iroseka.hcb" `
  --steps 900000 --click 600 --quiet --text-out out.txt
```

然后对比 `ref-900k.txt`。

## 时间线

- 早期版本（484 行，含「未实现的 Test 返回 0」的缺陷）在同一参数下输出 130 行，已作废。
- 当前版本修正了三处：未实现的 `*Test` 命令返回 1、`V3DMotionTest` 返回 1、`MovieState` 返回 1（跳过 WMV 开场）。这三处都是**为了越过等待循环**，不是凭空猜测：原版里对应动画/视频播放结束就会返回「完成」。
- 冻结时间：2026-11-23 22:40 左右。

## 修订：argc 表（2026-11-23 22:50）

`cmd_argc.json` 原先是从 exe 注册表推出来的，其中 6 个 id 的口径与 `.hcb` 命令表不一致：

| id | 命令 | 旧值 | 新值（=hcb 表） |
|---|---|---:|---:|
| 49 | MotionMoveStop | 6 | 1 |
| 51 | MotionMoveR | 1 | 6 |
| 52 | MotionMoveRStop | 8 | 1 |
| 54 | MotionMoveS2 | 1 | 8 |
| 94 | SaveThumbSize | 3 | 2 |
| 95 | SaveData | 2 | 3 |

引擎在加载 hcb 时会用命令表校验 argc（不匹配会报「引数の数が違います」），所以**以 hcb 表为准**。

改完后重跑：**900k 与 2M 的输出与旧基准逐字节相同**（0 行差异），因此 `ref-900k.txt` / `ref-2m.txt` 保持不变，只有 `cmd_argc.json` 的哈希变了。

## 注意

- `fvm_ref.py` 依赖同目录的 `cmd_table.json` 与 `cmd_argc.json`。
- 原型仍在 `fvp/work/fvm.py` 继续演进，但**那份不再是验收基准**。