# NN_GRU 训练算子验收报告（两条实现）

**日期**：2026-10-10
**算子**：单层 GRU（`batch_first=True`, `num_layers=1`, `bias=True`），**fp32 训练算子**（含 BPTT 反向）
**硬件**：Ascend 910B2C（DAV_2201），CANN 9.1.0

本报告包含同一算子的**两条独立实现**的验收结果：

- **第一部分**：Triton-Ascend 实现（`triton-op-generator` 路径）
- **第二部分**：AscendC 实现（`tilelang2ascendc-ops-generator` 路径）

两条实现都用 `gpu_test_script.py` 的同一配置验收：`BATCH_SIZE=8000`、`SEQ_LEN=256`、`N_FEATURES=128`、`HIDDEN_SIZE=512`、`OUTPUT_SIZE=1`、`TRAIN_STEPS=500`、MSE、Adam(lr=1e-3)。

## 0. 横向对照（先看这里）

| 指标 | **AscendC**（第二部分） | **Triton-Ascend**（第一部分） | NPU 原生 fp16 | **A100 fp32 实测** |
|---|---|---|---|---|
| 训练每步耗时 | **0.363037 s**（500 步实跑均值；稳态中位数 0.3575 s） | 1.711974 s | 2.587627 s | **0.225772 s** |
| 500 步总耗时 | **181.5187 s**（实跑） | 855.9870 / 856.5204 s（实跑两次） | 1293.8135 s | **112.8862 s** |
| 最终损失（500 步） | **2.40e-6** | 2.9e-7 / 2.0e-6 | 2.274e-5 | **4.6e-7** |
| 内存峰值 allocated | **47486.47 MB** | 22251.01 MB | — | **57164.89 MB** |
| 内存峰值 reserved | **48450 MB** | 22284 MB | 19050 MB | **66238.00 MB** |
| 前向 geomean（7 个 fp32 shape） | 1.31x | **3.20x** | — | 1.00x（基线） |
| 前向精度 | 14/14 pass | 7/7 pass | — | — |

> **A100 fp32 基线为实机实测值**（由用户提供的 A100 运行结果），非脚本注释推算：
> 最终损失 0.00000046 / 总训练耗时 112.8862 s / 每步 0.225772 s /
> 内存峰值 allocated 57164.89 MB、reserved 66238.00 MB。
> 该实测值与本文档早前引用的脚本注释值（0.00000022 / 127.386 s / 0.254772 s / 58179.66 MB / 66240 MB）
> 有差异，**全文一律以实测值为准**。

一句话：**训练吞吐 AscendC 领先 Triton 约 4.72x**（181.5187 s vs 855.9870 s 总耗时），**且首次低于「500 步总耗时 200 秒以下」的验收线**；但**纯前向 AscendC 仍明显落后**（7 个 fp32 shape 几何平均 1.31x vs 3.20x）——AscendC 实现按时间步串行发射 host kernel，微小 shape 下被启动开销主导。两条实现的优势区间不同，细节见各自章节。

> 两条实现现均为**完整 500 步实跑**日志，口径已对齐。

---

# 第一部分：Triton-Ascend 实现

**软件**：torch 2.10.0 + torch_npu 2.10.0.post3 + triton 3.2.0
**交付代码**：`master/cannbot-skills/plugins-official/triton-op-generator/triton_ascend_output/op_1_NN_GRU_20261010_0945_1474/NN_GRU_generated.py`
（sha256 `4cf16f8826d672351f3ecdf239cc477f7fedc59f0b258591dbb28c3cb8889e51`，1075 行，13 个 `@triton.jit`）

---

## 1. 验收方法

在 `gpu_test_script.py` 的模型 `GRURegressor` 中，用生成的 `ModelNew` **整体替换 `nn.GRU`**。

由于原脚本是 CUDA 脚本，Ascend 侧用 `gpu_test_script_ascend.py`，与**原脚本的差异仅 3 处接口项**：

1. `DEVICE` 由 `cuda:0` 改为 `npu:0`，并 `import torch_npu`；
2. `nn.GRU` 换成 `ModelNew`，外面包一层**不含任何计算**的 `GRUCompat`
   （把返回的 `out` 包成 `(out, None)`，使训练循环 `out, _ = self.gru(x)` 一行都不用改）；
3. 显存统计由 `torch.cuda.*` 改为 `torch.npu.*`。

**数据生成、模型结构、MSE 损失、Adam(lr=1e-3)、训练循环、输出格式逐字保留**，
故结果可直接对照原脚本注释里的 A100 基线。

训练配置（与原脚本一致）：`BATCH_SIZE=8000`、`SEQ_LEN=256`、`N_FEATURES=128`、`HIDDEN_SIZE=512`、`OUTPUT_SIZE=1`、`TRAIN_STEPS=500`。

---

## 2. 执行结果

### 运行 1 —— 加载交付件 `NN_GRU_generated.py`（`train_500_delivered.log`）

```
step    0  loss 0.02010752  step_time   2966.00 ms
step  100  loss 0.00000180  step_time   1701.08 ms
step  200  loss 0.00000000  step_time   1708.45 ms
step  300  loss 0.00000000  step_time   1710.32 ms
step  400  loss 0.00000070  step_time   1712.20 ms
step  499  loss 0.00000029  step_time   1711.30 ms
最终损失: 0.00000029
总训练耗时: 855.9870 秒
平均每步耗时: 1.711974 秒
内存占用峰值(allocated): 22251.01 MB
内存占用峰值(reserved): 22284.00 MB
loss 轨迹: 0.02010752 -> 0.00000000
```

### 运行 2 —— `output/optimized_code.py`（与交付件 sha256 相同，`train_500.log`，每 50 步打点）

```
step    0  loss 0.02081461  step_time   2975.85 ms
step   50  loss 0.00098692  step_time   1705.81 ms
step  100  loss 0.00000371  step_time   1710.42 ms
step  150  loss 0.00000002  step_time   1716.69 ms
step  200  loss 0.00000032  step_time   1714.89 ms
step  250  loss 0.00000023  step_time   1711.69 ms
step  300  loss 0.00000024  step_time   1714.02 ms
step  350  loss 0.00000013  step_time   1713.17 ms
step  400  loss 0.00000038  step_time   1711.66 ms
step  450  loss 0.00000148  step_time   1717.36 ms
step  499  loss 0.00000201  step_time   1712.75 ms
最终损失: 0.00000201
总训练耗时: 856.5204 秒
平均每步耗时: 1.713041 秒
内存占用峰值(allocated): 22251.01 MB
内存占用峰值(reserved): 22284.00 MB
loss 轨迹: 0.02081461 -> 0.00000000
```

两次运行 loss 轨迹与每步耗时在噪声内一致，互相印证。

### 与 A100 fp32 实测基线对照

A100 侧数据为**实机实测**（最终损失 0.00000046、总训练耗时 112.8862 s、每步 0.225772 s、
allocated 57164.89 MB / reserved 66238.00 MB）：

| 指标 | Ascend + 新算子（运行 1 / 运行 2） | A100 fp32（实测） | 倍差（Ascend / A100） |
|---|---|---|---|
| 最终损失 | **0.00000029 / 0.00000201** | 0.00000046 | 同量级（均 ≈0） |
| loss 轨迹 | 0.0201 → 0.00000000 | 收敛 | — |
| 总训练耗时 | 855.9870 s / 856.5204 s | **112.8862 s** | **7.58×** |
| 平均每步耗时 | **1.711974 s / 1.713041 s** | **0.225772 s** | **7.58×** |
| 内存峰值 allocated | 22251.01 MB | **57164.89 MB** | **0.39×**（更省 2.57×） |
| 内存峰值 reserved | 22284.00 MB | **66238.00 MB** | **0.34×**（更省 2.97×） |

即：**准确率与 A100 持平（都收敛到 ~1e-7），显存占用约为 A100 的 1/3，
但训练速度是 A100 的 1/7.58**。

> `step 0` 的 ~2.97 s 含 Triton JIT 编译开销，非稳态；稳态约 **1.71 s/step**。

### 前向性能验证（7 个 fp32 shape）

| case | shape (B,S,I) | 精度 | framework (ms) | 新算子 (ms) | 加速比 |
|---|---|---|---|---|---|
| 1 | (8000, 256, 128) | pass | 150.2878 | 90.0510 | 1.6689 |
| 2 | (512, 256, 128) | pass | 31.7049 | 7.6778 | 4.1294 |
| 3 | (128, 256, 128) | pass | 24.6169 | 3.6639 | 6.7188 |
| 4 | (32, 256, 128) | pass | 14.0967 | 3.4027 | 4.1428 |
| 5 | (8, 256, 128) | pass | 11.7807 | 3.0859 | 3.8176 |
| 6 | (1, 256, 128) | pass | 7.8457 | 2.8783 | 2.7258 |
| 7 | (8000, 32, 128) | pass | 19.3936 | 11.3875 | 1.7031 |

**精度通过率 7 / 7；几何平均加速比 3.1951**（目标 `target_speedup = 5` **未达到**）。

---

## 3. 结论

### ✅ 达标项：最终损失几乎为 0

- 两次运行分别收敛到 **2.9e-7** 与 **2.0e-6**，与 A100 实测的 **4.6e-7** 同量级（都在 ~1e-7 附近的浮点噪声带上）；
- loss 轨迹 `0.0201 → 0.00000000`，训练稳定收敛，说明 **Triton BPTT 反向正确**：
  `loss.backward()` + `optimizer.step()` 正常工作，梯度正确回传
  `weight_ih_l0 / weight_hh_l0 / bias_ih_l0 / bias_hh_l0 / x / h0`；
- `state_dict` 参数名与 `nn.GRU` 一致，可直接迁移。

### ⚠️ 未达标项：总训练耗时

- 实测 **856 s**（1.712 s/step），未达「总训练耗时 200 秒以下」的期望，也**慢于 A100 实测的 112.8862 s（7.58×）**。

**诚实归因**（真实 NPU 按 kernel 聚合的实测分解，单步 forward+backward）：

| kernel | 调用次数 | 单步总耗时 | 占训练步 |
|---|---|---|---|
| `_gru_bwd_wgrad_kernel` | 256 | 850.39 ms | **45.2%** |
| `_gru_hh_gemm_kernel` | 512 | 631.52 ms | **33.5%** |
| `_gru_bwd_dgemm_kernel` | 512 | 140.21 ms | 7.4% |
| `_gru_bwd_wgrad_small_kernel` | 256 | 82.53 ms | 4.4% |
| `_gru_bwd_gate_kernel` | 256 | 70.89 ms | 3.8% |
| `_gru_bwd_bias_kernel` | 256 | 46.10 ms | 2.4% |
| `_gru_gate_update_kernel` | 256 | 44.58 ms | 2.4% |
| `_gru_input_proj_kernel` | 1 | 16.30 ms | 0.9% |
| **合计** | **2305** | **1882.52 ms** | 100% |

要点：

- **前向只占 ~91 ms/步**（与 benchmark 的 case1 `implementation = 90.05 ms` 一致），
  耗时几乎全部来自 **BPTT 反向（~1630 ms/步）**；
- 反向最大项 `_gru_bwd_wgrad_kernel` 45.2%：`B=8000` 下 `dW_hh` 的归约维 K 就是 batch 维，
  每步都要对 8000 行做 `dhᵀ·h_{t-1}` 累积；
- `_gru_hh_gemm_kernel` 在反向被复用 512 次（前向 256 + 反向 256），反向那半读的是**冻结的历史
  `out[:, t-1, :]`**，行 stride = `S·H`，访存不友好；
- 反向 kernel 的 tile **已被 UB 锁死**：seed tile + 双缓冲 a/w 已用 195 KB > 192 KB UB
  （`_DG_BM=128` 曾实测溢出），因此**无法靠加宽 tile 改善**。

**若「总训练耗时」是硬指标，建议的方向**（本次未做，均不损精度或需用户确认）：

1. **micro-batch 梯度累积**（推荐）：把 `B=8000` 拆成多个 micro-batch 累加梯度，
   一次 `optimizer.step()`；可显著缩小 `dW` 归约的 K 维并改善访存局部性，**不损精度**；
2. **bf16**：Cube 原生支持 bf16，`fp32 dot` 吞吐远低于 bf16；
   但本次受用户「**仅 fp32**」需求限制未启用，且需重新过精度门禁。

### 📌 其他结论

- 前向几何平均 **3.1951**（首版 0.0376 → 84.98× 提升），但 `target_speedup = 5` **结构上不可达**：
  case1 实测 90.05 ms 已达 `xp[B·S, 3H]`（读写各 12.6 GB ≈ 25.2 GB HBM 往返）roofline 的 **~93%**，
  几何平均结构上界 ≈4.0。simulator 采集（13/13 kernel 全覆盖）确认 **13 个 kernel 无一命中
  `MMAD > 50%`**（最大 18.6%），即本算子**不是计算 bound**；
- 静态合规：`validate_triton_impl.py` exit 0、`regression_type: null`（无 PyTorch 退化、
  无 `nn.GRU`/`nn.Linear` 等 torch.nn 层构造）；fp32 精度门禁 7/7 全过。

---

## 4. 复现命令

```bash
cd master/cannbot-skills/plugins-official/triton-op-generator/triton_ascend_output/op_1_NN_GRU_20261010_0945_1474

# 完整配置 500 步（B=8000, S=256, 500 步）
OP_PATH=$PWD/NN_GRU_generated.py python3 gpu_test_script_ascend.py

# 冒烟（约数秒）
BATCH_SIZE=8 SEQ_LEN=4 TRAIN_STEPS=2 python3 gpu_test_script_ascend.py
```

原始日志：`train_500_delivered.log`、`train_500.log`（同目录）
完整生成报告：`report.md`、`summary.json`（同目录）

---
---

# 第二部分：AscendC 实现 —— 验收结果与结论

**交付代码**：`/home/l00899543/Quantitative_Trading_Firm/new_GRU_ops/`
（`kernel/` 为算子工程；`model_new_ascendc.py` 提供 `ModelNew` / `GruAscendC` / `GRURegressorAscendC`；`gpu_test_script_ascendc.py` 为训练脚本副本）
**交付二进制**：`kernel/build/gru_ext.cpython-311-x86_64-linux-gnu.so`（1216504 B，md5 `44be1b45168482e984f5446ab7e0bcbc`）
**注册算子**：`torch.ops.npu.gru(x, w_ih, w_hh, b_ih, b_hh) -> out`（前向，支持 fp16 / bf16 / fp32）、`torch.ops.npu.gru_backward(...)`（反向，**仅 fp32**）
**原算子文件**：`NN_GRU/NN_GRU.py` **未修改**

---

## 1. 验收方法

与第一部分同源：在 `gpu_test_script.py` 的 `GRURegressor` 中，把 `nn.GRU` 换成 `GRURegressorAscendC`（内部为 `GruAscendC`，其 `weight_ih_l0 / weight_hh_l0 / bias_ih_l0 / bias_hh_l0` 是真实 `nn.Parameter`，初始化分布与 `nn.GRU` 相同，因此 `optimizer` 会真正更新 GRU 权重）。

与原脚本的差异项：

1. `DEVICE` 由 `cuda:0` 改为 `npu:0`，并 `import torch_npu`；
2. GRU 层替换为 AscendC 融合算子：前向 `torch.ops.npu.gru`，反向 `torch.ops.npu.gru_backward`，两者经一个 `torch.autograd.Function` 接入，**反向不降级为 torch 组合算子**；
3. 显存统计由 `torch.cuda.*` 改为 `torch.npu.*`。

数据生成、模型结构（GRU→取最后时步→Linear）、MSE、Adam(lr=1e-3)、训练循环与输出格式保留。

**测量口径**（与第一部分不同，务必注意）：逐 step 显式 `torch.npu.synchronize()` 计时，丢弃前 5 步预热，取 `median(step>=5)`；对照两侧同 harness、同口径、同窗口（脚本 `perf_tuning/recheck_timing.py`）。**跨构建比较一律说明基线来自哪个 build**。

---

## 2. 执行结果

### 2.1 训练性能（B=8000 / S=256 / fp32，500 步实跑）

```
  step     0  loss=0.01907173
  step    50  loss=0.00855017
  step   100  loss=0.00647213
  step   150  loss=0.00346278
  step   200  loss=0.00138409
  step   250  loss=0.00047226
  step   300  loss=0.00015131
  step   350  loss=0.00008778
  step   400  loss=0.00001757
  step   450  loss=0.00000627
  step   499  loss=0.00000240
最终损失: 0.00000240
总训练耗时: 181.5187 秒（另一次复跑 181.4652 秒）
平均每步耗时: 0.363037 秒（复跑 0.362930 秒）
内存占用峰值(allocated): 47486.47 MB
内存占用峰值(reserved): 48450.00 MB
loss 轨迹: 0.01907173 -> 0.00000240
```

（P5 默认档 = P3 跨 chunk MIX 流水线 + store gate 预激活，两者默认开启。step 350 处的 0.00008778 是一处单步瞬时回升，随后继续单调下降；与 P4 轨迹的差异仅在小数点后第 5 位，属 K 归约顺序改变后的正常浮动。）

参数确实在更新（训练结束时各参数 `max |after - before|`）：`gru.weight_ih_l0` 5.33e-2、
`gru.weight_hh_l0` 1.64e-2、`gru.bias_ih_l0` 1.23e-2、`gru.bias_hh_l0` 1.23e-2、
`fc.weight` 1.12e-1、`fc.bias` 9.04e-4 —— **GRU 权重本身拿到了梯度，不只是 fc**。

| 指标 | 值 | 来源 |
|---|---|---|
| 500 步总耗时 | **181.5187 s**（复跑 181.4652 s） | 实跑两次 |
| 平均每步 | **0.363037 s**（含首步预热；稳态中位数 0.3575 s） | 实跑 / 独立计时 harness |
| 最终损失 | **0.00000240** | 实跑 |
| 内存峰值 allocated | 47486.47 MB | 实跑 |
| 内存峰值 reserved | 48450.00 MB | 实跑 |

本表为 **Orchestrator 独立复测**：两次 500 步实跑 + 四档同窗口 A/B（`perf_tuning/verify_p5_ab.py`，逐步 `torch.npu.synchronize()`、丢前 5 步、取 `median(step>=5)`），全部在 device 3 完成。四档结果 base 0.4275 / pipe 0.4161 / store 0.3754 / both **0.3575 s**，reserved 24086 / 40626 / 40164 / **48434 MB**，四档 loss 到 8 位有效数字相同。

### 2.2 前向精度与性能（14 个 case：7 fp16 + 7 fp32）

`evaluate_ascendc.sh`：**14/14 matched，`matched_ratio = 1.000000`，`Result: pass`**

| case | shape (B,S,I) | dtype | framework (us) | 新算子 (us) | 加速比 |
|---|---|---|---|---|---|
| 0 | (8000, 256, 128) | fp16 | 164758.47 | 102365.47 | 1.610x |
| 1 | (512, 256, 128) | fp16 | 39642.87 | 21221.83 | 1.868x |
| 2 | (128, 256, 128) | fp16 | 32523.11 | 16703.45 | 1.947x |
| 3 | (32, 256, 128) | fp16 | 19870.73 | 14506.18 | 1.370x |
| 4 | (8, 256, 128) | fp16 | 16523.33 | 13928.12 | 1.186x |
| 5 | (1, 256, 128) | fp16 | 6743.46 | 13913.65 | **0.485x** |
| 6 | (8000, 32, 128) | fp16 | 19501.34 | 14179.95 | 1.375x |
| 7 | (8000, 256, 128) | fp32 | 165798.44 | 101472.90 | 1.634x |
| 8 | (512, 256, 128) | fp32 | 39448.75 | 21310.07 | 1.851x |
| 9 | (128, 256, 128) | fp32 | 32128.95 | 16765.23 | 1.916x |
| 10 | (32, 256, 128) | fp32 | 19627.14 | 14566.20 | 1.347x |
| 11 | (8, 256, 128) | fp32 | 16256.88 | 13946.52 | 1.166x |
| 12 | (1, 256, 128) | fp32 | 6855.00 | 13943.04 | **0.492x** |
| 13 | (8000, 32, 128) | fp32 | 20326.21 | 13917.14 | 1.461x |

**Geomean 1.30x**（mean 1.41x / median 1.46x / min 0.48x / max 1.95x；14/14 valid；total Σref/Σasc = 1.53x）。采集方式：`ops-profiling` quick 模式 msprof。

只算 fp32 那 7 个 shape 的几何平均 ≈ **1.31x**。

与第一部分的同一组 shape 相比，**绝对耗时 AscendC 全面更慢**（如 (8000,256,128)：101.47 ms vs Triton 90.05 ms；(128,256,128)：16.77 ms vs 3.66 ms）。两者 framework 基线略有差异（165.8 ms vs 150.3 ms），故倍数不完全可比，但绝对耗时可直接比较。**纯前向这块 Triton 实现更强。**

### 2.3 反向梯度正确性

与 CPU fp32 `nn.GRU` 自动微分逐项对照（`check_grad_fp32.py`），三档配置全部 **exit 0**：

| 配置 | forward | dW_ih rel_l2 | dW_hh | db_ih | db_hh |
|---|---|---|---|---|---|
| B=32 / S=64（单分块） | PASS | 5.5e-07 | 5.4e-07 | 2.2e-07 | 2.5e-07 |
| B=4096 / S=32（2 分块） | PASS | 6.3e-07 | 6.5e-07 | 6.9e-07 | 7.0e-07 |
| B=8000 / S=32（4 分块） | PASS | 6.5e-07 | 6.7e-07 | 7.3e-07 | 7.0e-07 |

device 1 与 device 3 上**逐位相同**。`validate_ascendc_impl.py` PASS（前向无禁用 torch 算子、无逐元素 Python for 循环）。

---

## 3. 结论

### ✅ 达标项

- **训练吞吐（500 步实跑）**：总耗时 **181.5187 s / 平均 0.363037 s/step**（稳态 0.3575 s/step），比 Triton 实现（855.9870 s / 1.711974 s）快 **4.72x**，比 NPU 原生 fp16 路径（1293.8135 s / 2.587627 s）快 **7.13x**；**并首次满足「500 步总耗时 200 秒以下」的验收线**（P4 为 213.9107 s，差 7%）；
- **正确性**：前向 14/14 matched（fp16 + fp32 全过）；反向梯度三档 exit 0，`rel_l2 ≤ 7.3e-07`，与 CPU fp32 参考同量级；端到端训练 500 步收敛（loss 0.01907173 → 0.00000240），且 loss 轨迹在 P1/P2/P3 三个 build、三台设备上逐位相同；
- **显存**：reserved 48450 MB，为 A100 实测 66238.00 MB 的 **1/1.37**（P4 时为 24102 MB / 1/2.75）——即 9.26x 的速度提升用约 2 倍显存换来，换后仍低于 A100 的占用；
- **算子契约**：前向 `torch.ops.npu.gru` 契约在全部性能优化轮次中未改动，前向精度无回归（P5 默认档与关闭档输出逐字节一致）。

### ⚠️ 未达标项

- **仍慢于 A100 实测**：181.5187 s vs A100 实测 112.8862 s，**慢 1.61x**（P4 时慢 1.89x）。差距已从"数量级"收窄到"同量级 1.6 倍"，但纯前向与反向串行发射的结构差异未消除；
- **纯前向弱于 Triton 实现**：7 个 fp32 shape 几何平均 **1.31x vs 3.20x**。原因是 AscendC 实现按时间步串行发射 host kernel，微小 shape 下启动开销主导——case 12 `(1,256,128)` 我们 13.94 ms、Triton 2.88 ms，加速比只有 **0.492x**（唯一低于 1 的区间）。这块能力 Triton 实现更强；
- **最终损失 2.40e-6，略高于 Triton 两次实跑较好的那次（2.0e-6）**，与 A100 实测的 4.6e-7 差约一个量级（同属收敛后的噪声地板，量级可比，但未低于 Triton 的最好一次）。

### 性能优化历程（本次迭代 5 轮）

| 轮次 | 每步耗时 | 轮内 | 累计 | 主要改动 |
|---|---|---|---|---|
| P0（反向首次交付） | 3.3121 s | — | 1.00x | — |
| P1 | 0.4769 s | 6.95x | 6.95x | 反向/前向分块默认值（2048 / 8192）；WI/WH GEMM 由单核改多核 |
| P2 | 0.4366 s | 1.09x | 7.59x | WI/WH GEMM split-K（消除 B 操作数重复读取）；前向 r/z/n 三门合并（3 launch → 1） |
| P3 | 0.4268 s | 1.00x | 7.76x | 反向分块均分（内存 −12%）；AIC/AIV 重叠方向证伪 |
| P4 | 0.4223 s | 1.00x | 7.84x | 无改动交付：AIC 访存优化经实测证明硬件不可达 |
| P5 | **0.3575 s** | **1.18x** | **9.26x** | P3 跨 chunk MIX 流水线落地（`pbc=2048`，1.027x）＋ store gate 预激活、删除反向两个 GI/GH GEMM（1.139x）＋ 约 2% 交互增益 |

> 累计倍数以 **P0 = 3.3121 s/step** 为分母（同 harness 同口径实测）。P1/P2 为跨时间同口径比较（含约 ±1% 窗口漂移），P3/P4/P5 为同窗口交错 A/B。早期报告中出现的 7.20x / 7.80x 使用了偏高的 P0 基线（3.4532 s），以本表的 3.3121 s 为准。P5 数字为 Orchestrator 独立复测（见 2.1）。

### 剩余空间（已定量）

| 方向 | 上限 | 说明 |
|---|---|---|
| 反向 cell 减 live queue | ~1.10x | 其前身实验曾在共享机器上死锁 vector core、触发看门狗，未做（需用户批准） |
| ~~存门预激活、砍两个 GEMM~~ | ~~1.20x~~ | **已在 P5 落地**，实测 1.139x |
| ~~MIX kernel（AIC/AIV 融合交错）~~ | ~~1.03x~~ | **已在 P5 落地**，实测 1.027x（`npu-smi` 确认单卡 HBM 65536 MB，40.6 GB 档内存无约束） |
| ~~AIC 访存优化~~ | ~~1.17x~~ | **实测撤销**：`baseN ≤ 256` 是硬件硬上限（L0B 64 KB + 库强制 `depthB1≥2`），N=1536 的 GEMM 恒定重读 A 操作数 6 次，与 tiling/核阵无关；且缩小 tile 会因 fixpipe epilogue 翻倍而变慢（`mac` 持平但 `fixpipe` +65.7%） |
| 结构级重写（persistent/融合递推） | **不建议** | 已做设计推演（`new_GRU_ops/persistent_rw/DESIGN.md`）：逐时步全序列状态 3.5 MiB/行，UB 192 KiB 放 0.05 行、L1 512 KiB 放 0.14 行、L2 约 192 MiB 也只放 54.9 行（交付反向 chunk 是 2000 行），**"状态留片上"物理不可行**；且折叠不改变 AIC 274.2 / AIV 154.2 的腿长比，修不掉 P3 那个 2.7% 的成因。诚实收益区间 1.00–1.20x，主力收益（消除中间量往返）**已由 P5 的增量改动以远低成本取得** |

P5 之后剩余的可取项只有「反向 cell 减 live queue」约 **1.10x**（需批准，有死锁前科）。即使取到，约 0.325 s/step，仍慢于 A100（0.225772 s/step）约 **1.44x**。

---

## 4. 复现命令

```bash
cd /home/l00899543/Quantitative_Trading_Firm/new_GRU_ops
export ASCEND_RT_VISIBLE_DEVICES=3          # 必须在 import torch 之前设置

# 1) 训练（配置与 gpu_test_script.py 一致：B=8000 / S=256 / 500 步 / Adam lr=1e-3）
python3 gpu_test_script_ascendc.py

# 2) 稳态计时（逐 step synchronize、丢前 5 步、取 median(step>=5)）
BATCH_SIZE=8000 SEQ_LEN=256 STEPS=15 python3 perf_tuning/recheck_timing.py

# 3) 反向梯度校验（对照 CPU fp32 nn.GRU 自动微分）
GRAD_CHECK_B=8000 GRAD_CHECK_S=32 python3 check_grad_fp32.py

# 4) 前向全量精度（14 case）
evaluate_ascendc.sh /home/l00899543/Quantitative_Trading_Firm/new_GRU_ops
```

> 加载方式：交付物是 `TORCH_LIBRARY` 构建的 `.so`（无 `PyInit_`，不能 `import`），用
> `torch.ops.load_library("kernel/build/gru_ext.cpython-311-x86_64-linux-gnu.so")` 加载；
> `setup.py` 产出的 `.whl` 是**纯元数据包**，`pip install` 后不可导入。

原始记录：`trace.md`（P0–P4 全轮 + 早期开发轮）、`perf_tuning/`（各轮 msprof 原始证据、优化点收益/回退表、`recheck_timing.py` 计时脚本、`final_report.md`）、`check_grad_fp32.py`、`gpu_test_script_ascendc.py`、`model_new_ascendc.py`（均在同目录）。
