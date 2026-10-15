# HWSimulation — Hull-White 短期利率蒙特卡洛 AscendC 算子

CUDA 算子 `HWMCKernel`（`<<<1, 16>>>`，一线程一路径，fp64）到华为 NPU 的 AscendC 迁移实现：
递推系数推导、时间轴顺序扫描、输出发射融合为**单次下发**的注册算子

```python
rate = torch.ops.npu.hwm(step, hwparams, accrual, f1, f2, nrnd)   # [NPath, MStep] fp32
```

业务背景：CallableCLN 定价程序（SOFR 折现 + BNP/BOC 信用，10Y 可赎回 CLN）Part IV 的
短期利率路径模拟阶段。完整迁移报告见 [`HWSimulation/cuda2npu_report.html`](HWSimulation/cuda2npu_report.html)。

## 目录结构

```
├── kernel_case.json            测试用例 HWSimulation_BOC_2024-07-24（输入 + 期望输出）
├── extract_kernel_case.py      用例提取脚本：纯 Python MT19937 + Acklam 逆正态，fp64 参考重建
├── callablecln.hpp             模型框架头
├── finmath/                    模型自带数学库（DMT 随机数 / DStat 统计 / ChronoTime）
└── HWSimulation/
    ├── model.py                PyTorch fp32 参考实现（精度基准）
    ├── model_new_ascendc.py    AscendC 算子封装（torch.ops.npu.hwm）
    ├── run_in_model.py         模型集成 runner：真实输入 → NPU 实跑 → 双基准比对
    ├── trace.md                7-Phase 开发过程记录
    ├── perf_report.md / performance.json / performance.log    性能测量结果
    ├── hwm_perf_cases.jsonl   性能采集脚本的用例桩
    ├── perf_tuning/SKIPPED.md  TileLang 阶段合法跳过的对照实验证据
    ├── cuda2npu_report.html    CUDA→NPU 迁移报告（功能/平台/精度/性能）
    └── kernel/                 AscendC 算子工程
        ├── CMakeLists.txt / setup.py
        ├── ops.h / register.cpp
        ├── op_host/hwm.cpp     host 侧：tiling（路径轴切分 AIV 核）+ 单次 launch
        ├── op_kernel/hwm.cpp   device 侧：CopyIn → Compute → CopyOut
        └── utils/torch_kernel_helper.h
```

## 环境要求

| 项 | 版本 |
|---|---|
| 芯片 | Ascend 910B2（dav-c220），其他 910B 系列可改 `SOC_VERSION` |
| CANN | 9.1.0（`ASCEND_HOME_PATH=/usr/local/Ascend/cann-9.1.0`） |
| Python | torch 2.10.0 + torch_npu |

## 编译

```bash
cd HWSimulation/kernel
mkdir -p build && cd build
cmake .. -DSOC_VERSION=Ascend910B2 \
         -DASCEND_CANN_PACKAGE_PATH=/usr/local/Ascend/cann-9.1.0 \
         -DCMAKE_BUILD_TYPE=Release
make -j
```

产出 `build/hwm_ext*.so`，`model_new_ascendc.py` 会自动 `torch.ops.load_library` 加载，
无需安装。（`setup.py` 打 wheel 存在已知打包缺陷——包内缺 `hwm/` 目录，建议直接走 cmake。）

## 运行与验证

```bash
# 算子单测：打印 rate 形状与前 4 步
ASCEND_RT_VISIBLE_DEVICES=3 python HWSimulation/model_new_ascendc.py

# 模型集成：真实输入 → NPU 实跑 → 与 expected_output / 框架控制台转储双比对
ASCEND_RT_VISIBLE_DEVICES=3 python HWSimulation/run_in_model.py
```

## 精度与性能摘要

- **精度 PASS**：容差匹配率 1.000000（门限 ≥ 0.99）；对 fp64 参考重建 max_abs_error
  1.294e-08，对框架控制台转储 6.788e-08（框架两基准互差 7.216e-08，算子误差落在基线
  自洽误差之内）。原 CUDA 为 fp64；Ascend 无 fp64 向量通路，参考与候选统一按 fp32 评估。
- **性能**：555.58 µs（PyTorch 参考）→ 10.74 µs（AscendC），geomean **51.73×**
  （NPU 3，msprof Task_Duration）。主要来自 10 次下发合并为 1 次的开销消除——算子规模
  仅 16×10，耗时贴近单次下发开销（同机 16 元素逐元素基线约 4.3 µs）。
