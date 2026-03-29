# HPF-shmem Benchmark

Demonstrates the advantage of XSched HPF (Highest Priority First) with shared memory: one high-priority CUDA process maintains predictable latency while 127 low-priority processes compete for the GPU.

## Design

- **High-priority process (1)**: **one** kernel launch per period, **~50ms sleep** between periods, workload tuned toward **~20ms GPU** per launch (`--kernel-ms 20`).
- **Low-priority processes (127)**: **one** launch per period, **~100ms sleep**, **~1ms** target (`--kernel-ms 1`).

`kernel_ms` in the CSV is **wall time for that single launch + `cudaStreamSynchronize`**, not the sleep. If your GPU is faster/slower, adjust **`--inner-repeat`** in `cuda_bench_app` (see `--help` via source) or tweak `--high-kernel-ms` / workload in code.
- With HPF-shmem, the high-priority process should get near-20ms kernel latency; low-priority processes are preempted when high-pri runs.

## Requirements

- NVIDIA GPU + CUDA
- XSched built with CUDA support (`make cuda` in `third_party/xsched`)

## Build

```bash
cd pmu_Roboflex/benchmark/xsched-shm-bench
make
```

## Run

```bash
# Run with XSched HPF-shmem (default)
python3 run_bench.py

# Custom options
python3 run_bench.py --xsched-path /path/to/xsched/output --duration 60 --num-low 127

# Quick test with fewer low-pri processes
python3 run_bench.py --num-low 8 --duration 30 --iterations 50

# Tune kernel duration for your GPU (default: high=20ms, low=1ms)
python3 run_bench.py --high-kernel-ms 50 --low-kernel-ms 2

# If high-priority stats are missing, increase stagger so high-pri finishes prepare first:
python3 run_bench.py --stagger-sec 5 --duration 120

# Baseline without XSched
python3 run_bench.py --no-xsched --num-low 8
```

## 双终端手动测试（`run_high_pri.sh` / `run_low_pri.sh`）

与 `run_bench.py` 使用相同的环境变量与 `cuda_bench_app` 参数；先启动 **xserver**（HPF + shmem），再开两个终端分别跑高/低优先级。

1. **终端 0（可选，单独窗口）** — 启动调度服务，例如：

   ```bash
   # 或使用仓库内脚本
   bash ../../script/startup_xsched_server.sh
   ```

   需设置 `XSCHED_HPF_SHM=ON`（见 `startup_xsched_server.sh`）。

2. **终端 1 — 高优先级**（单个进程，`XSCHED_AUTO_XQUEUE_PRIORITY=255`）：

   ```bash
   cd pmu_Roboflex/benchmark/xsched-shm-bench
   make
   chmod +x run_high_pri.sh run_low_pri.sh
   ./run_high_pri.sh
   # 或基线（无 XSched）: NO_XSCHED=1 ./run_high_pri.sh
   # 输出到文件: LOG=high.csv ./run_high_pri.sh
   ```

3. **终端 2 — 低优先级**（默认 127 个进程，`PRIORITY=0`，日志在 `logs_low_<pid>/`）：

   ```bash
   ./run_low_pri.sh
   # 少量进程试跑: NUM_LOW=8 ./run_low_pri.sh
   # 无 XSched: NO_XSCHED=1 NUM_LOW=8 ./run_low_pri.sh
   ```

说明：低优先级脚本会**并行**拉起多个子进程并将每个进程的 CSV 写到 `LOG_DIR`；高优先级脚本默认把 CSV **打印到当前终端**，需要保存时用 `LOG=high.csv`。

## Output

The script reports:

- **High-priority kernel latency**: mean (sensitive to outliers), **median**, stdev, p95, p99, **p99.9**, max (ms)
- **Low-priority kernel latency**: same
- **Overhead**: high-pri observed vs expected ~20ms
- **HPF effect**: ratio of high-pri vs low-pri average latency

### Statistics notes

- **p99 vs max**: With heavy-tailed latency (many short GPU syncs + rare long queue waits), **max** can be orders of magnitude above **p99**. Use **median** or **p99.9** to see the tail without a single worst sample dominating the story.
- **Mean** can be dominated by a few multi-second waits; compare **median** when evaluating “typical” behavior.
- Percentiles use **linear interpolation** between order statistics (similar to `numpy.percentile(..., method="linear")`).

## Reference

Based on [xsched examples/Linux/1_transparent_sched](../../third_party/xsched/examples/Linux/1_transparent_sched).

## Environment (set by run_bench.py)

When using XSched:
- `XSCHED_SCHEDULER=GLB`
- `XSCHED_AUTO_XQUEUE=ON`
- `XSCHED_HPF_SHM=ON` (xserver)
- `XSCHED_AUTO_XQUEUE_PRIORITY=255` (high) or `0` (low)
