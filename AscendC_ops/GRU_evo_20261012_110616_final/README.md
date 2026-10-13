# GRU 算子进化优化 — 最终有效代码

Ascend C GRU（融合前向 + 反向）算子的**最终最优变体**源码与报告副本。

| 项 | 值 |
|---|---|
| 最优变体 | `round_3/parallel_3`（世界模型节点 `x1_x1_x1`） |
| 来源 | `/workspace/Quantitave_firm/evo_kernel_a5_hf32/GRU_evo_20261012_110616/round_3/parallel_3/` |
| 目标平台 | Ascend950PR_9579（arch35 / dav-c310），CANN 9.1.0 |
| 算子接口 | 与基线**完全一致**（未改签名 / 未改测试用例） |

## 成绩

| 口径 | 值 |
|---|---|
| 算子级（4 用例均值） | **2.0501x**（baseline 65.946 ms → 32.167 ms） |
| 算子级（case 0 生产形状 B=8000/S=256/I=128/H=512） | 2.107x（234.637 → 111.373 ms） |
| 端到端（真训练回路 500 步，同 harness） | **2.073x** |
| **对 A100（0.225772 s/step，同 harness 同配置）** | **1.971x** |
| 精度 | 4/4 通过，最坏 `rel_l2 ≈ 6.0e-04`（判据 1e-2，约 17x 余量） |

优化手法（详见 `implementation_note.txt`）：把混合精度下沉到反向路径——`dha/dhb` 累加器与 DH-GEMM 输出降到 fp16（L0C 仍 fp32 累加，仅 WI/WH 的 split-K 保留 fp32），并使 `gru_bwd_colsum_partial` 软件流水化（qLoad 双缓冲）。

## 目录结构

```
kernel/                     AscendC 内核源码（不含 build/）
  op_kernel/                内核实现：8 个 .h/.cpp（本次改动集中于此）
  op_host/                  host 侧 tiling/注册（gru.cpp）
  bench/                    开发期基准脚本
  CMakeLists.txt  setup.py  register.cpp  ops.h  build_a5.sh
model.py                    算子描述（PyTorch 参考实现）
model_new_ascendc.py        AscendC 实现入口（调用 torch.ops.npu.gru / gru_backward）
GRU.json (+ .bak)           测试用例（4 个）；.bak 为全量备份，用于完整性校验
call_spec.json              用例参数规格
verify_and_bench.py         算子级评估（精度 + 计时）
eval_kernel.sh              单变体完整评估流水线（clean build → msprof → 精度/计时）
gen_golden.py               生成 torch-fp32 金标
implementation_note.txt     **本次优化的改动说明与调优过程**
evaluation_results.json     **op 级 2.0501x 的评估证据**
reports/
  report_GRU_evo_vs_A100.html          最优 vs A100 对照报告（含数据格式/口径说明章）
  evolution-report_GRU_20261012_110616.html   进化过程全报告（逐变体轨迹、代码 diff、世界模型）
  final-summary_GRU_20261012_110616.md  补充文档（性能汇总 / 环境说明 / §11 最终成绩）
```

## 未包含（有意排除）

- `kernel/build/` —— 编译产物（`gru_ext*.so`、`*.o`），可用下方命令重建
- `golden/`（4.2 GB）—— 生成的金标张量数据，非代码
- `profiling/` —— msprof 采集输出
- 其余变体与中间轮次 —— 见原进化目录

## 重新构建

```bash
cd kernel && bash build_a5.sh --clean     # 产出 build/gru_ext*.so
```

`build_a5.sh` 已把本容器的两个环境陷阱固定在内：`SOC_VERSION` 必须为 `Ascend950PR_9579`（镜像自带的全局值是 950DT SKU，用错会静默编出错架构内核），`python3` 必须解析到 `/usr/local/python3.12.13`（镜像 `/usr/bin/python3` 无 torch）。

## 重新验证

```bash
bash eval_kernel.sh .                      # clean build + 精度 + 计时 → evaluation_results.json
python3 verify_and_bench.py                # 仅算子级精度/计时
```

> ⚠️ `eval_kernel.sh` 内的全局 NPU 锁路径**硬编码**指向原进化目录
> （`/workspace/Quantitave_firm/evo_kernel_a5_hf32/GRU_evo_20261012_110616/.npu_eval.lock`）。
> 该目录仍存在，故在此副本中也能运行，但锁的是原目录——若要独立使用，请改这一行。
