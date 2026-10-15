# 性能评估结果

- **Operator**: HWSimulation
- **Device**: npu:3 (source=cli)
- **Warmup**: 1
- **Repeats**: 1
- **Seed**: 0
- **Timing method**: msprof.quick.Task_Duration

## 性能对比

| Case | Shape | DType | 自定义算子(us) | 标杆(us) | 加速比 |
| ---- | ----- | ----- | ------------- | -------- | -------------- |
| 0 | [1] | int32 | 10.74 | 555.58 | 51.730 |

## 全量汇总

| 指标 | 值 |
| ---- | -- |
| 用例数 | 1 |
| 平均加速比（>1 表示自定义算子更快） | 51.730 |
| 标杆几何平均耗时 (us) | 555.58 |
| 自定义算子几何平均耗时 (us) | 10.74 |
| 自定义算子更优（比值>1） | 1 |
| 标杆更优（比值<1） | 0 |

### 按数据类型汇总

| DType | 用例数 | 平均加速比 | 自定义算子更优 | 标杆更优 |
| ----- | ------ | ------------------- | ------------- | -------- |
| int32 | 1 | 51.730 | 1 | 0 |

## 简短分析

- 平均加速比 51.730 大于 1，自定义算子整体有优势。
- 详细瓶颈分析见 msprof 归档目录（op_summary_*.csv + summary.txt）。

## 深度瓶颈分析

如需进一步分析性能瓶颈（各流水线利用率、核间负载均衡、主 Bound 判定），可运行：
```bash
python3 ${SKILL_PATH}/scripts/msprof_perf_summary.py ./PROF_GROUP_* HWSimulation
```

或参考 `ops-profiling/references/optimization_quickref.md` 获取优化建议。
