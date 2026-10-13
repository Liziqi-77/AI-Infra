# GRU 算子进化优化 — 最终总结（补充文档）

> ⚠️ **本文档写于第 2 轮结束时（2026-10-12），其性能结论已被后续的第 3 轮超越——请以 §11 为准。**
> 第 3 轮已于 2026-10-13 补跑并**达成 2.0x 目标**：op 级 **2.0501x**、端到端 **2.073x**（对 A100 **1.971x**）。
> 面向"最优 vs A100"的最终报告见 `report_GRU_evo_vs_A100.html`；正文第 1–10 章保留为**第 2 轮时点的历史记录**（其中"不再执行第 3 轮""1.9632x 即最终成绩"等表述已被 §11 推翻）。

> 本文件是对 `evolution-report_GRU_20261012_110616.html` 的补充，不是替代。
> 报告负责逐变体轨迹、代码 diff、世界模型决策树；本文件补齐**性能汇总 / A100 对照 / 环境停止说明 / 数据质量注记 / 断点续跑路径**。
> 所有数字均来自落盘产物（`baseline_evaluation.json`、各 `evaluation_results.json`、`state.json`、`world_model.json`），并已人工复核。
> 生成时间：2026-10-12 ｜ 设备：Ascend950PR_9579（device 0）

---

## 1. 结论

- **最优变体 `round_2/parallel_3`：4 用例平均算子 step 墙钟 65.946 ms → 33.592 ms（融合 GRU 前向+反向，npu:0 实测），加速比 1.9632x，精度 4/4 通过（门 `rel_l2 ≤ 1e-2`）。**
- **未达 2.0x 目标，差 1.8%**；但**生产形状（case 0，B=8000/S=256/I=128）单用例已达 2.005x**。
- **端到端（真训练回路：GRU→Linear + MSELoss + Adam，500 步，生产形状）实测 1.982x**（0.237486 → 0.119816 s/step），与算子级 1.9632x 一致；**对 A100 同口径 1.884x**。详见 §10——口径与 A100 完全对齐，是该结论最强的证据。
- 在第 **2 / 3** 轮因**容器内存耗尽**（环境问题，非算法或精度问题）提前停止；**该阻塞已自行解除，第 3 轮具备续跑条件（无需重启容器），见 §4.2 / §7**。
- 用户于 2026-10-12 复核后**决定就此收尾、不再执行第 3 轮（显式取消）**。因此 **1.9632x / 4 用例通过即为本次最终成绩**，未达 2x 目标的 1.8% 缺口就此结案。
- 取消已如实记录：`world_model.json.session.early_termination_reason` 保留，`state.json.stage = aborted`（经授权的 hook 顺序修复后，该终态不再被 `infer` 覆盖）；`requested_rounds=3`、`actual_rounds_completed=2` **均未改动**——未通过改写 round 数来消除 R6，细节见 §8.1 / §8.2。

---

## 2. 最终性能

### 2.1 逐用例

| 用例 | shape (B,S,I) | dtype | PyTorch 参考 | 基线内核 | 进化后 | 单用例加速比 |
|---|---|---|---|---|---|---|
| **0（生产形状）** | 8000, 256, 128 | fp32 | 702.356 ms | 234.637 ms | **117.042 ms** | **2.005x** |
| 1 | 1024, 128, 128 | fp32 | 73.781 ms | 23.623 ms | 12.635 ms | 1.870x |
| 2 | 256, 64, 256 | fp32 | 18.192 ms | 3.989 ms | 3.185 ms | 1.252x |
| 3 | 64, 32, 128 | fp16 | 6.150 ms | 1.534 ms | 1.503 ms | 1.021x |
| **平均** | — | — | **200.119 ms** | **65.946 ms** | **33.592 ms** | **1.9632x** |

- 对照关系：`vs 基线内核 = 1.9632x`；`基线内核 vs PyTorch 参考 = 3.035x`；`进化后 vs PyTorch 参考 = 5.957x`。
- **口径（已按源码核对更正）**：表内所有时间是 `verify_and_bench.py:116-130` 用 `time.perf_counter()` 包住**融合 GRU 前向+反向算子调用**（含 host launch + `torch.npu.synchronize()`，warmup 2 / iters 3 **取最小**）的**墙钟**；**不是** msprof 核时间（msprof 是另一步 best-effort，仅用于取 `pipeline` 字段）。`baseline_time_us = 65945.72 µs` 是同一脚本在基线内核上测的 4 用例均值——即"融合 fwd+bwd 算子 step"，**不是**整模型训练 step。
- **"PyTorch 参考"列跑在同一块 NPU 上**（`gen_golden.py:59` `dev = torch.device("npu:0")`，PyTorch eager fp32 + autograd），**不是** A100、也不是 CPU。故"vs PyTorch 5.957x"是**同设备**对照，**不可**读作"比 A100 快 5.96 倍"（对 A100 的对照见 §3，用的是 A100 自己的记录值）。

### 2.2 精度

- **判据**：harness 的通过门是每个输出 `rel_l2 ≤ 1e-2` **且** `cos ≥ 0.99`（`verify_and_bench.py:48-49`），5 个输出（`out, dW_ih, dW_hh, db_ih, db_hh`）全过才算该用例通过。
- **注意门的松紧**：本门比 `HF32_REPORT.md` 用的 fp32 门（`rtol=1e-3, atol=2^-13`）**松约一个量级**。所以本文件的"4/4 通过"与基线报告 §4(b) 的 fp32 门 **FAIL** 并不矛盾——**是两个不同的门**，不可混引。
- 4/4 用例通过（`all cases within tol`），`measurement_quality = good`，编译成功。
- 全部输出 `rel_l2 ≤ 5.82e-04`，属 **TF32 量级**（fp32 应为 ~1e-7 量级；此值正是 fp16 输入 + fp32 累加的预期误差），远在 1e-2 门与 fp32 门之内。
- 数值方案：fp16 输入 + **fp32 L0C 累加**。

### 2.3 加速来源

fp16 门控预激活（gi/gh 走 fp16、L0C fp32 累加）→ cube 不再饱和、AIV 的 MTE2 搬运量减半。
最佳策略分析中的 profiling：`aic_mac_ratio=0.31521 | aic_mte2_ratio=0.38863 | aiv_mte2_ratio=0.29532 | aiv_mte3_ratio=0.08589 | bottleneck=balanced`。
代码改动 7 个文件（`op_kernel/gru_cube.cpp`、`gru_vector.cpp`、`gru_bwd_cube.cpp`、`gru_bwd_vector.cpp`、`gru_bwd_bodies.h`、`gru_tiling_data.h`、`op_host/gru.cpp`），逐文件 diff 见进化报告。

### 2.4 测量口径全貌（供复核，"这个数到底怎么来的"）

| 项 | 值 |
|---|---|
| **被测单元** | **融合 GRU 前向 + 反向算子**（`torch.ops.npu.gru` + `torch.ops.npu.gru_backward`），由 `model_new_ascendc.ModelNew` 薄封装——**不是**端到端训练模型（无优化器 / 损失 / 数据管线 / 多层网络） |
| **被测对象** | 该变体自己构建的 `kernel/build/gru_ext.cpython-312-x86_64-linux-gnu.so`（已核对该路径确实指向本变体） |
| **设备** | Ascend950PR_9579，npu:0（`eval_kernel.sh` 中强制 `SOC_VERSION=Ascend950PR_9579`、`ASCEND_RT_VISIBLE_DEVICES=0`、全局 flock 串行化） |
| **计时方法** | `time.perf_counter()` 包住算子调用 + `torch.npu.synchronize()`；warmup 2 / iters 3 **取最小值**（`verify_and_bench.py:116-130`）→ **墙钟，非 msprof 核时间** |
| **精度判据** | `rel_l2 ≤ 1e-2` 且 `cos ≥ 0.99`（`verify_and_bench.py:48-49`），5 个输出 |
| **金标来源** | PyTorch eager fp32 + `torch.autograd.grad`（`model.py:gru_recurrence`），在 npu:0 上生成后冻结为 `golden/case_*.pt`（`gen_golden.py`）——**模型级金标**，非单算子 |
| **用例** | 4 个合成用例，输入由 `torch.Generator(seed=1234)` 确定性生成；case 0 = 生产形状 B=8000/S=256/I=128/H=512 |
| **加速比定义** | `baseline_time_us ÷ evolved 均值`，两侧**同脚本、同设备、同用例、同判据**（基线侧见 `baseline_evaluation.json`） |
| **执行者** | 各变体评估由**进化 Subagent** 在 NPU 上实际执行（证据：`eval1-3.log`、`build.log`、`profiling/`）；**本汇总文档未重新运行 benchmark**，审计的是落盘产物（日志 / `evaluation_results.json` / `.so` 路径 / 用例 md5 / 两侧均值自洽性） |

> **可复现**：**端到端复跑已完成，见 §10**（真训练回路实测 1.982x / 对 A100 1.884x）。若还需复核算子级的 1.9632x，可直接重跑 `round_2/parallel_3` 的 `verify_and_bench.py`（.so 已构建，无需重建）；注意单次评估峰值 ~24 GiB（§4.1），跑前先核对余量。

---

## 3. 与 A100 的对照（同形状 B=8000, S=256, I=128, H=512）

> **已有更强的证据**：§10 用**同一个 harness** 跑了真训练回路 500 步，口径与 A100 **完全对齐**（同形状/同步数/同损失/同优化器/同为每步墙钟）→ 进化后 **1.884x**（本方 0.119816 s/step）。下面这张表是**算子级**对照，保留以便追溯其来源与差异。

| 实现 | 该形状耗时 | 相对 A100 |
|---|---|---|
| A100（仓库记录基线，225.772 ms/step） | 225.772 ms | 1.00x |
| A5 基线内核（本次进化起点） | 234.637 ms | 0.96x（慢 3.8%） |
| **A5 进化后（round_2/parallel_3）** | **117.042 ms** | **1.93x（快 1.93 倍）** |

若改用 `trace.md` 中的另一 A100 参考值 0.2548 s/step，则为 **2.18x**。

### 必须同时声明的四点

1. **A100 数字的 dtype 标签在仓库里自相矛盾**：`0.225772 s/step` 在 `NN_GRU_ascendc_final/NN_GRU_acceptance_report.md` 标为 **fp32 实测**，在 `kernel_a5_hf32/HF32_REPORT.md`、`optimization_log.md` 标为 **A100 TF32 目标 227.9 ms**——同一测量、两种标签。
   另有记录称真 fp32 的 A100 约 `0.8214 s/step`（按此则进化后 7.02x、进化前基线也已 3.50x），**但该数值在当前目录树中检索不到，本文不作为结论**，需确认来源后再引用。
2. **测量口径不完全对齐（已按源码更正）**：我方 117.042 ms 是 `verify_and_bench.py` 用 `time.perf_counter()` 包住**融合 GRU 前向+反向两个算子调用**（含 host launch + `npu.synchronize()`，3 次取最小）的**墙钟**——**不是** msprof 核时间。A100 侧 225.772 ms 是 500 步训练**每步墙钟**（含优化器 / 损失 / 数据管线）。所以差异仍在：我方缺优化器/损失/数据管线开销，A100 侧含之 → 该比值**偏乐观**，但原因与我先前所述不同。
3. **精度要对等比较**：进化后内核为 fp16 输入 + fp32 累加（`rel_l2 ≤ 5.82e-04`，TF32 量级），对标 **A100 TF32** 才同类；与"真 fp32 的 A100"比速度属不同数值精度下的比较。
4. **不要混入 910B2C 的数据**：`NN_GRU_ascendc_final/` 中"A5 比 A100 慢 7.58x / 1.6x"等结论基于 **Ascend 910B2C (DAV_2201)** 与当时的代码版本，与 Ascend950PR 的当前树既非同一设备也非同一版本，不可与上表并列。

---

## 4. 为什么在第 2 轮停止（环境问题，非算子问题）

### 4.1 停止时的现场

- 容器内存为 **cgroup v1**：`/sys/fs/cgroup/memory/memory.limit_in_bytes = 32.00 GiB`，当时 `memory.usage_in_bytes ≈ 27.4 GiB` → **余量仅 ~4.7 GiB**。
  宿主机本身有 ~1.4 TB 空闲，**瓶颈是 cgroup 上限，不是宿主机**。
  （注意：v2 路径 `/sys/fs/cgroup/memory.max`、`memory.current` 在本容器**不存在**，需读 v1 路径。）
- 根因：pid **1378870**（`round_2/parallel_2/verify_and_bench.py`）被 SIGKILL 后卡在退出路径的**不可中断 D 态**（`wchan = rq_qos_wait`），仍占用 **~25 GB** 匿名页。`PF_EXITING` 任务被 OOM reaper 跳过，容器内无法回收（cgroup 文件只读、无 `CAP_SYS_PTRACE`、宿主 swap 仅 ~3 GB）。
- 而单次评估峰值需 **~24 GiB**（groups + gd + golden + 多份 (a-b) 拷贝）→ **任何后续评估都跑不完**，且可能在直接回收中再制造一个楔死进程。
- 另有第二个楔死进程 pid **1449095**（孤立的 `cp -r shared/. round_3/parallel_0/`，PPID 1，D 态，仅 4 MB 占用）。
- 影响面：`round_2/parallel_2`（节点 `n1_cont`）因此从未被有效评估——**其实现与构建已完成，只是没跑成**。

### 4.2 后续复核：阻塞已自行解除（2026-10-12 晚，本文写作时）

**两个楔死进程均已被回收，环境已恢复，无需重启容器：**

| 指标 | 停止时 | 现在 |
|---|---|---|
| `memory.usage_in_bytes` | ~27.4 GiB | **3.17 GiB** |
| 可用余量 | ~4.7 GiB | **28.83 GiB** |
| pid 1378870 / 1449095 | D 态，共占 ~25 GB | **均已消失** |
| 全系统 D 态进程 | 有 | **无** |
| NPU 0 | — | 空闲，`No running processes`，HBM 5249/131072 MB |

即：余量 28.83 GiB > 单次评估峰值 ~24 GiB，**第 3 轮现在具备续跑条件，不需要容器重启**。
（回收原因未确证——可能是内核最终完成了 aio/块层 teardown，或宿主侧对 cgroup 做了回收。同一失败模式**仍可能复发**，续跑时建议串行执行评估、每次评估前先查余量。）

> **读数提醒**：cgroup v1 的 `memory.usage_in_bytes` **含可回收 page cache**。2026-10-13 复查时它显示 9.30 GiB，但拆开是 `cache 6.58 GiB + rss 2.71 GiB`——真正的匿名占用只有 ~2.7 GiB，page cache 在压力下会被回收。判断"能不能跑评估"要看 **rss**，不要被 usage 的涨落吓到。

---

## 5. 落地 / 复现

- 最优内核树：`round_2/parallel_3/kernel/`（与 `shared/kernel/` 的差异即全部改动；逐文件 diff 见进化报告）
- 其评估脚本与用例：同目录下 `model.py`、`model_new_ascendc.py`、`eval_kernel.sh`、`GRU.json`（4 用例）、`verify_and_bench.py`
- **⚠️ 进化报告末尾"应用最优变体"的命令不可用**：它引用 `round_2/parallel_3/modified_files/...`，而该目录**不存在**，照抄会报 No such file。
  可用的等价做法：把 `round_2/parallel_3/kernel/` 下的 7 个变更文件覆盖到目标树（变更文件清单见第 2.3 节），或先 `diff -r shared/kernel round_2/parallel_3/kernel` 自取补丁。

---

## 6. 数据质量注记（引用报告前请先读这段）

| # | 位置 | 问题 | 结论 |
|---|---|---|---|
| 1 | 报告 `失败/无效轮次分析` | 写 **`2-V2 精度失败`** | **错误**。世界模型中该节点记为 `failure_type=environment`（`status=failed`，原因：评估进程楔死 / 容器内存耗尽）。该变体 4 个用例**一个都没跑成**，不存在精度回归。 |
| 2 | 报告 `资源消耗统计 → 词元用量统计` | 全为 0 | 未采集到 token 数据，可忽略该节。 |
| 3 | 报告 `硬件环境 → 峰值带宽 51.15 GB/s` | 与器件量级不符 | **勿引用**。`platform_config/Ascend950PR_9579.ini` 无 bandwidth 字段，`51.15 = ddr_rate(31) × 1650 MHz / 1000`，是 910B 系列推导公式的产物。ini 实测可比引用的值为：`ai_core_cnt=28`、`l2_size=128 MiB`、`memory_size=128 GiB`（与 `npu-smi` 的 131072 MB 一致）。 |
| 4 | 报告 `应用最优变体` | 命令引用的 `modified_files/` 不存在 | 见第 5 节。 |

已人工复核**无误**的部分：最优变体的加速比与用例通过数、case 文件完整性（`GRU.json` 与 `.bak` md5 一致 `677e25db550f249ee9e56ca85caecf74`，4 用例）、内核确有实质改动（非空跑）、`world_model.json ≡ world_model_final.json`、`validate-diagnosis --strict` 通过（9 节点 0 问题）。

---

## 7. 断点续跑路径

- **真实记录（未做任何篡改）**：`world_model.json.session` 写入 `actual_rounds_completed=2 / requested_rounds=3`，`early_termination_reason = environment/container-memory: ...`。
- `state.json.stage` 当前为 `round_checkpoint`：注意**这不是被手工写入的值**，而是 Stop hook 每次运行 `state_ops.py infer` 从文件系统推导出来的（实际 2 轮 < 请求 3 轮 → 推导为 `round_checkpoint`）。
- **第 3 轮选点已算好并保持 open**：`n5_cont_s1`、`n8_s1`、`n7`、`x1_x1_x1`、`x0_x1_x1`；`next_round_hint` = 优先 **D1（fp16 混合精度）**、避免新增 cell 队列。
- **补齐最后 1.8% 的最高价值线索**：把 fp16 混合精度**从门控预激活扩展到反向 GEMM**（节点 `n7` / D1）。
- **未评估的高价值候选**：`round_2/parallel_4/kernel_fp16bwd_ok/`（全 fp16 反向，报告口径 ~1.9x，从未被有效评估，容器恢复后可直接评估）。
- **另一处可复用成果**：`round_2/parallel_2/kernel/` 是"已修好但未验证"的 2-D grid 版本；其父节点的已验证树在 `round_1/parallel_2/kernel/`（含 `gru.cpp:632` 一行 host 修复）。
- **续跑前提已满足**：容器内存已回收（余量 28.83 GiB > 评估峰值 ~24 GiB，见 4.2），NPU 0 空闲；**无需重启容器**。执行时请串行评估并逐次核对余量。

---

## 8. 操作注记（工具链缺陷，与算子无关）

本会话收尾时插件的 Stop hook 反复以 **R6**（`actual_rounds_completed=2 < requested_rounds=3`）拦截退出，且**在原理上无法满足**：

- `hooks/loop-stop.sh:46-49` 在判规则前先执行 `state_ops.py infer`，用文件系统覆盖 `stage`；
- `state_ops.py:523-544`（`_decide_stage`）只有在 `max_round >= requested_rounds` 时才可能返回 `done`/`finalize`，否则（此处 2 < 3）恒返回 `round_checkpoint`；
- 而 `hooks/lib/state.sh:127-134` 中 R6 的豁免名单是 `done|aborted|finalize|report`，**不含 `round_checkpoint`**。
- 实测佐证：Agent 写入的 `aborted` 在磁盘上已被 hook 覆盖为 `round_checkpoint`。
- 即：对"请求 N 轮、实际 M<N 轮、因环境提前取消"的运行，无论写什么终态都会被覆盖。唯一能让它通过的方式是伪造一个已完成的 `round_3`，或把 `requested_rounds` 改为 2 —— **本次均未采用**，记录保持真实。
- 若需根治：让 hook 优先读取显式的取消标记，或让 `_decide_stage` 认可显式取消；应急出口为 `LINGXI_LOOP_HOOK_DISABLE=1`（需落在 harness 的环境中）。

### 8.1 显式取消为什么也过不了 R6（2026-10-12 复核，逐条按源码核对）

用户已决定就此收尾并要求显式取消。但**在不篡改记录的前提下，没有任何状态能让 R6 通过**：

| 途径 | 结果 |
|---|---|
| `state_ops.py write-stage --stage aborted` | `aborted` 本在 R6 豁免名单内，但 hook 在判规则**前**先跑 `infer`（`hooks/loop-stop.sh:46-49`），把 `stage` 覆盖成推导值 |
| 靠 `infer` 推出终态 | `_decide_stage`（`scripts/state_ops.py:523-544`）**永不返回 `aborted`**；此处 `actual(2)==max_round(2) < requested(3)` → 恒为 `round_checkpoint`（541 行） |
| 给 `infer` 加"保留终态"参数 | `infer` 只有 `--evo-dir/--print-state/--quiet/--bootstrap`，无此选项（`state_ops.py:888-899`） |
| 把 `requested_rounds` 3 改为 2 | 可让推导出 `done` 从而绕过——但这**改写了"用户请求 3 轮"的历史**，正是 R6 要防的"悄悄缩范围"，属禁止的"绕过进化世界模型状态"，**未采用** |
| 把 `actual_rounds_completed` 2 改为 3 | 等于凭空捏造一轮，**未采用** |
| 删改 session anchor | anchor 为 `/workspace/Quantitave_firm/cannbot-skills/output/.ops-evo_current_session_GRU.json`，删它即让 hook 找不到活跃 loop——同属规避，**未采用** |

**结论**：`loop-stop.sh:67-74` 的终止态豁免（明确含 `aborted`）因 46 行的前置 `infer` 沦为**死代码**——插件缺陷。停止原因已如实落在 `world_model.json.session.early_termination_reason`（`infer` 不触碰该字段，故稳定可查）；但该字段记的是**停止当时**的原因（环境/内存），其后"阻塞已解除、用户决定收尾"这一节**无法**再写入该字段（`wm_ops.py` 无对应操作，手工改 wm 被约束禁止），故记录于本文件 §1 / §4.2。

**要真正放开这次 stop，只有两条不篡改记录的路**：

1. **用插件自带的应急出口**：在 **Claude Code 进程所在的环境**里 `export LINGXI_LOOP_HOOK_DISABLE=1`。
   ⚠️ 注意：在工具调用的 shell 里 export **无效**——hook 的父进程是 CLI，不继承该 shell 的环境。
2. **修这个 hook（一行级）**：让 hook 在 `infer` 之前先尊重显式终态（`done`/`aborted`），或让 `cmd_infer` 不覆盖已有终态。

### 8.2 处理结果（2026-10-13，用户明确授权后采用第 2 条）

改动落在 `hooks/loop-stop.sh`（**插件仓库源码，在 `{output_dir}` 之外——经用户明确授权**），**只调整 hook 的判定顺序，未改动任何规则**：

- 新增：先读一次 stage；若已是显式终态 `done`/`aborted`，**跳过 `infer`**，直接走既有的终止态豁免（`loop-stop.sh:67-74`，该名单本就包含 `aborted`）。
- 非终态时 `infer` 照旧执行 → R2/R3/R4 仍以文件系统推导为准，**防护未被削弱**。

**验证（两条分支均实测）**：

| 测试 | 写入 stage | hook 退出码 | 事后 stage | 结论 |
|---|---|---|---|---|
| 取消路径 | `aborted` | **0（放行）** | `aborted`（未被覆盖） | 显式取消被尊重 |
| 反例：非终态 | `round_generate` | **2（拦截，R6）** | `round_checkpoint`（被 infer 覆盖） | infer 仍生效、规则未被削弱 |

**取消记录（全程走插件自带 CLI，未手工改状态文件、未改写任何数字）**：`state.json.stage = aborted`；`world_model.json.session` 的 `requested_rounds=3`、`actual_rounds_completed=2`、`early_termination_reason` **原样保留**。这正是 §8.1 表中"未采用"诸途径的反面——**记录真实，且 R6 不再拦截**。

> 备用出口仍有效：`export LINGXI_LOOP_HOOK_DISABLE=1`（须落在 Claude Code 进程的环境里；工具 shell 中 export 无效）。

---

## 9. 产物清单

```
/workspace/Quantitave_firm/evo_kernel_a5_hf32/GRU_evo_20261012_110616/
├── evolution-report_GRU_20261012_110616.html   # 主报告（169 KB，逐变体轨迹 / diff / 世界模型）
├── final-summary_GRU_20261012_110616.md        # 本文件
├── world_model.json / world_model_final.json   # 决策树（两者一致）
├── baseline_evaluation.json                    # 基线性能（baseline_time_us = 65945.72 µs）
├── evolution_log.txt                           # 轮次摘要 + 环境停止记录
├── ENV_AND_SETUP_LOG.md                        # 环境改动日志
└── round_{1,2}/parallel_{0..4}/                # 各变体：内核树 / 评估结果 / 实现说明
```

---

## 10. 端到端验证（真训练回路，2026-10-13 实测）

**方法**：直接复用产出 A100 记录值的**同一个** harness `NN_GRU_ascendc_final/gpu_test_script_ascendc.py`（**未改一行**），仅通过其 `GRU_SO` 环境变量切换 .so。回路为 `GRU 层（torch.ops.npu.gru + gru_backward）→ Linear 头` + MSELoss + Adam，生产形状 B=8000 / S=256 / I=128 / H=512，**500 步**；两侧 `DATA_SEED=0`、`torch.manual_seed(0)` 一致，故数据与初始化逐位相同。

驱动脚本 `e2e/run_e2e.sh`（写在 `{output_dir}` 内）｜原始日志 `e2e/logs/{base,evo}_e2e.log`。

| 指标 | 基线 .so | 最优变体 .so | 加速 |
|---|---|---|---|
| **总耗时（500 步）** | 118.7428 s | **59.9079 s** | **1.982x** |
| **每步（回路内计时）** | 0.237486 s | **0.119816 s** | **1.982x** |
| 每步（bench 段，warmup 3 / 20 步） | 237.065 ms | 119.390 ms | 1.986x |
| ├ forward | 85.335 ms | 39.514 ms | 2.160x |
| └ backward + optimizer + zero_grad | 151.730 ms | 79.876 ms | 1.900x |
| 峰值显存 reserved | 62108 MB | 35692 MB | **省 1.74x** |
| 模块级数值检查（vs CPU `nn.GRU` 参考） | PASS（max_abs 6.775e-05） | PASS（max_abs 8.755e-05） | — |
| 末步 loss | 2.501e-05 | 1.526e-05 | — |

**主要结论**：端到端加速 **1.982x**，与算子级 4 用例均值 **1.9632x 一致（略高）**——算子级增益在真实训练回路中**没有被 host / 优化器开销吃掉**。副作用：显存占用降到基线的 1/1.74。

**与 A100 的对照现在口径完全对齐**（同 harness、同形状、同 500 步、同损失/优化器/数据、同为每步墙钟）：

| | s/step | 相对 A100 |
|---|---|---|
| A100（仓库记录值 0.225772） | 0.225772 | 1.000x |
| A5 基线 .so | 0.237486 | 0.951x（慢 4.9%） |
| **A5 进化后 .so** | **0.119816** | **1.884x（快 88.4%）** |

这比 §3 基于算子级的对照**更强**：§3 里"我方口径不含优化器/数据管线开销"的不对齐，在端到端下**不再存在**。

### 必须一并声明的三点

1. **loss 轨迹**：两侧前 100 步相对差 ≤3%；末端在 ~1e-5..4e-5 处摆动（step 300 时 evo/base=1.50，step 499 时 0.61）。该量级是**定点 8000 样本"记忆化"的精度噪声底**（此 harness 无小批量、无留出集，500 步反复拟合同一批数据）→ **尾部差异不可解读为一方更优**，两侧数值等价。
2. **loss 底 vs A100**：A100 记录末损失 4.6e-07，比两侧 A5（~1e-5 量级）低 1~2 个数量级，与"本内核为 TF32 量级精度、噪声底更高"一致；但**不足以单独定论**，取决于 A100 那次究竟是 fp32 还是 TF32（该标签在仓库中自相矛盾，见 §3 第 1 点）。**要点：进化后不比 A5 基线差**（末损失还略低）。
3. **证据等级**：本节的数字是**本轮由 Orchestrator 亲自运行**得到的（此前算子级数字由进化 Subagent 采集，我只做落盘产物审计）——即"复现"而非"审计"。

> 复跑方式：`bash {output_dir}/e2e/run_e2e.sh both`（`base`/`evo` 可单跑；`TRAIN_STEPS` 可覆盖步数）。

---

## 11. 后续：第 3 轮补跑（2026-10-13）——目标达成

> §1–§10 记录的是**第 2 轮结束时**的状态。本节记录此后发生的事，**结论以本节为准**。

### 11.1 背景

环境阻塞已自行解除（§4.2），用户于 2026-10-13 授权**续跑第 3 轮**（`requested_rounds=3` 的剩余预算，从 `stage=aborted` 断点恢复）。故 §1 中"不再执行第 3 轮""1.9632x 即最终成绩"等表述**已作废**。

### 11.2 结果

| 口径 | 第 2 轮最优 `round_2/parallel_3` | **第 3 轮最优 `round_3/parallel_3`** |
|---|---|---|
| op 级（4 用例均值） | 1.9632x | **2.0501x** |
| op 级（case 0 生产形状） | 2.005x | **2.107x**（234.637 → 111.373 ms） |
| 端到端（500 步，生产形状） | 1.982x | **2.073x** |
| 对 A100（0.225772 s/step） | 1.884x | **1.971x** |
| 最坏 `rel_l2` | 5.82e-04 | 6.01e-04（判据 1e-2，~17x 余量） |

**端到端实测（真训练回路，同 harness、同 seed、仅换 `.so`）**：

| | A5 基线 .so | **第 3 轮最优 .so** | 倍数 |
|---|---|---|---|
| 总耗时（500 步） | 118.7428 s | **57.2756 s** | **2.073x** |
| 每步（回路内计时） | 0.237486 s | **0.114551 s** | **2.073x** |
| forward / bwd+opt | 85.3 / 151.7 ms | **38.1 / 76.0 ms** | — |
| 峰值 reserved | 62108 MB | **35692 MB** | 1.74x 更省 |
| 模块级数值检查 | PASS 6.775e-05 | PASS 8.755e-05 | — |
| 末步 loss | 2.501e-05 | 1.356e-05 | — |

**对 A100**：A100 0.225772 s/step = 1.000x → 第 3 轮最优 **1.971x**。已**越过 2.0x 目标**（对进化起点 2.073x）。

### 11.3 做法

第 3 轮最优节点 `x1_x1_x1` 仅改 `kernel/` 实现，接口/签名/测试用例未动：把混合精度**继续下沉到反向路径**——`dha/dhb` 累加器与 DH-GEMM 输出由 fp32 收窄到 fp16 端到端，仅 WI/WH 的 `transA=T` split-K 保留 fp32 累加；另加 colsum 预取式双缓冲。这正是世界模型在 `x1_x1` 上标注的首要杠杆（`next_round_hint: 优先 D1`）。

本轮 5 个变体 5/5 编译通过、精度 4/4 通过：`parallel_3`(x1_x1_x1) 2.0501x、`parallel_2`(n7) 1.9941x、`parallel_4`(x0_x1_x1) 1.9238x、`parallel_0`(n5_cont_s1) 1.1293x、`parallel_1`(n8_s1) 1.0546x。

### 11.4 核验

- `state.json`：`stage=done`、round **3/3**（非提前终止）；`world_model_final.json`：`best_score=2.0501`、`requested_rounds=3 / actual_rounds_completed=3`。
- 测试用例完整性：`GRU.json` 的 md5 在 `round_3/parallel_3` / `shared` / `baseline` **三者一致**（`677e25db…`）→ 用例集未被改动，加速比可比。
- §11.2 的端到端数字由 **Orchestrator 亲自复跑**（`e2e/run_e2e.sh evo3`），非转述子 agent 的算子级数；日志 `e2e/logs/evo3_e2e.log`。

### 11.5 是否还有空间

第 3 轮后世界模型刷新的**共同残余**：255+256 次 per-step 微 GEMM 的 **kernel launch 延迟**（约占总时 60%，属 latency 受限而非带宽受限）与 colsum 的 ~5.2%；下一轮首选策略为 **P54（launch 合并/削减）**。但该族**有失败前科**——round 1 的 n4 用 MIX 跨 chunk 融合压启动次数时遭遇 **aicore timeout 死锁**被证伪，`GRU_BWD_PIPE` 亦栽于同类方向。故属「高杠杆、高风险」，且收益边际已收窄。

**用户于 2026-10-13 复核后决定：就此收尾。** 本次最终成绩为 **op 级 2.0501x / 端到端 2.073x（对 A100 1.971x）**。

