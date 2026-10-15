# NN_GRU 版本归档

归档时间：2026-10-15

## 版本说明

| 目录 | 来源 | 定位 |
|---|---|---|
| `v1_functional_new_GRU_ops/` | `Quantitative_Trading_Firm/new_GRU_ops/` | 最初跑通功能的版本（用户指定） |
| `v2_a100_half_NN_GRU_ascendc_optimized/` | `Quantitative_Trading_Firm/NN_GRU_ascendc_optimized/` | A100 1/2 性能版本（B=8000 训练 step ≈ 0.425 s，约为 A100 实测 0.2258 s/step 的 53%） |

## 内容范围

每个版本保留：`kernel/`（op_host + op_kernel + 构建脚本，源码态）、`design/`（TileLang 设计文档）、
`model.py` / `model_new_ascendc.py` / `model_new_tilelang.py`（模型侧封装）、
`check_grad_fp32.py` / `gpu_test_script_ascendc.py`（验证脚本）、
`perf_train_bench.py` / `perf_kernel_profile.py`（基准脚本，仅代码）、`NN_GRU.json`（测试用例）。

按用户要求未复制（性能采集数据/过程参考/构建产物）：
`perf_tuning/`、`extra-info/`、`prof_step/`、`persistent_rw/`、`performance.json`、
`performance.log`、`perf_report.md`、`trace.md`、`fusion_result.json`、`NN_GRU.json.bak`、
`kernel/{build,dist,gru.egg-info}`、`__pycache__/`。

> 注意：两个版本均为源码态，未带编译产物；如需运行，先在各自 `kernel/` 下重新构建。
