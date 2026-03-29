#!/usr/bin/env python3
"""
HPF-shmem benchmark: 1 high-priority + N low-priority CUDA processes.

Each CSV row's kernel_ms is the **wall-clock time for one CUDA kernel launch +
cudaStreamSynchronize** in cuda_bench_app (not the sleep between samples).

Defaults: high-pri ~50ms between launches / ~20ms target kernel; low-pri ~100ms / ~1ms.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# Default paths relative to this script
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_XSCHED_PATH = SCRIPT_DIR / "../../third_party/xsched/output"
DEFAULT_APP = SCRIPT_DIR / "cuda_bench_app"


def get_xsched_env(xsched_path: Path, priority: str) -> dict:
    """Build env for xsched-scheduled process."""
    lib_path = xsched_path / "lib"
    env = os.environ.copy()
    env["XSCHED_SCHEDULER"] = "GLB"
    env["XSCHED_AUTO_XQUEUE"] = "ON"
    env["XSCHED_AUTO_XQUEUE_LEVEL"] = "1"
    env["XSCHED_AUTO_XQUEUE_THRESHOLD"] = "16"
    env["XSCHED_AUTO_XQUEUE_BATCH_SIZE"] = "8"
    env["XSCHED_AUTO_XQUEUE_PRIORITY"] = "255" if priority == "high" else "0"
    env["LD_LIBRARY_PATH"] = f"{lib_path}:{env.get('LD_LIBRARY_PATH', '')}"
    return env


def run_bench(
    xsched_path: Path,
    app_path: Path,
    duration_sec: float,
    num_low: int,
    use_xsched: bool,
    high_interval_ms: int = 50,
    low_interval_ms: int = 100,
    high_kernel_ms: int = 20,
    low_kernel_ms: int = 1,
    iterations: int = 200,
    stagger_sec: float = 3.0,
):
    # Build iterations to run for duration (approx)
    # High runs every 50ms -> ~20 inv/s; low every 100ms -> ~10 inv/s
    # Use fixed iterations and let duration be max(run time)
    procs = []
    out_files = []
    fds_to_close = []

    # 1 high-priority
    fd, path = tempfile.mkstemp(suffix="_high.csv")
    out_files.append(("high", path))
    high_env = get_xsched_env(xsched_path, "high") if use_xsched else os.environ.copy()
    high_cmd = [
        str(app_path),
        "--priority", "high",
        "--kernel-ms", str(high_kernel_ms),
        "--interval-ms", str(high_interval_ms),  # sleep after each sample (~50ms)
        "--iterations", str(iterations),
        "--id", "0",
    ]
    f = os.fdopen(fd, "w")
    fds_to_close.append(f)
    p = subprocess.Popen(
        high_cmd,
        env=high_env,
        stdout=f,
        stderr=subprocess.DEVNULL,
        cwd=str(SCRIPT_DIR),
    )
    procs.append(("high", 0, p))

    # Let high-pri finish heavy prepare() + first kernels before 127 lows contend for CPU/GPU.
    # Otherwise high's CSV may stay empty under timeout / OOM while lows already stream rows.
    # if stagger_sec > 0:
    #     time.sleep(stagger_sec)

    # N low-priority
    for i in range(1, num_low + 1):
        fd, path = tempfile.mkstemp(suffix=f"_low_{i}.csv")
        out_files.append(("low", path))
        low_env = get_xsched_env(xsched_path, "low") if use_xsched else os.environ.copy()
        low_cmd = [
            str(app_path),
            "--priority", "low",
            "--kernel-ms", str(low_kernel_ms),
            "--interval-ms", str(low_interval_ms),
            "--iterations", str(iterations),
            "--id", str(i),
        ]
        f = os.fdopen(fd, "w")
        fds_to_close.append(f)
        p = subprocess.Popen(
            low_cmd,
            env=low_env,
            stdout=f,
            stderr=subprocess.DEVNULL,
            cwd=str(SCRIPT_DIR),
        )
        procs.append(("low", i, p))

    # Wait for duration or until all done
    start = time.time()
    while time.time() - start < duration_sec:
        if all(p.poll() is not None for _, _, p in procs):
            break
        time.sleep(0.5)
    else:
        # Timeout: kill all
        for _, _, p in procs:
            if p.poll() is None:
                p.terminate()
        time.sleep(1)
        for _, _, p in procs:
            if p.poll() is None:
                p.kill()

    for f in fds_to_close:
        try:
            f.close()
        except OSError:
            pass

    # Parse results (prefer CSV column parts[1] over file label)
    high_times = []
    low_times = []
    for label, path in out_files:
        try:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if not line or line.startswith("proc_id"):
                        continue
                    parts = line.split(",")
                    if len(parts) >= 5:
                        try:
                            km = float(parts[3])
                            pri = parts[1].strip().lower()
                            if pri == "high":
                                high_times.append(km)
                            elif pri == "low":
                                low_times.append(km)
                            elif label == "high":
                                high_times.append(km)
                            else:
                                low_times.append(km)
                        except (ValueError, IndexError):
                            pass
        except OSError:
            pass
        try:
            os.unlink(path)
        except OSError:
            pass

    return high_times, low_times


def stats(data):
    if not data:
        return {}
    s = sorted(data)
    n = len(s)
    return {
        "count": n,
        "mean": sum(s) / n,
        "p50": s[n // 2] if n else 0,
        "p99": s[int(n * 0.99)] if n > 1 else s[0],
        "min": s[0],
        "max": s[-1],
    }


def main():
    ap = argparse.ArgumentParser(description="HPF-shmem benchmark")
    ap.add_argument("--xsched-path", type=Path, default=DEFAULT_XSCHED_PATH,
                    help="XSched install path")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="Max run duration in seconds")
    ap.add_argument("--num-low", type=int, default=16,
                    help="Number of low-priority processes")
    ap.add_argument("--no-xsched", action="store_true",
                    help="Run without XSched (baseline)")
    ap.add_argument("--iterations", type=int, default=2000,
                    help="Kernel iterations per process")
    ap.add_argument("--high-kernel-ms", type=int, default=1,
                    help="Target kernel time for high-pri (ms)")
    ap.add_argument("--low-kernel-ms", type=int, default=4,
                    help="Target kernel time for low-pri (ms)")
    ap.add_argument("--high-interval-ms", type=int, default=50,
                    help="Sleep between high-pri kernel samples (ms)")
    ap.add_argument("--low-interval-ms", type=int, default=200,
                    help="Sleep between low-pri kernel samples (ms)")
    ap.add_argument("--build", action="store_true", help="Build cuda_bench_app only")
    ap.add_argument(
        "--stagger-sec",
        type=float,
        default=3.0,
        help="Sleep after starting high-pri before spawning lows (0=disable). "
        "Avoids empty high-pri stats when duration is short or GPU is busy.",
    )
    args = ap.parse_args()

    xsched_path = args.xsched_path.resolve()
    app_path = DEFAULT_APP.resolve()

    # Build
    subprocess.run(["make", "-C", str(SCRIPT_DIR)], check=True)
    if not app_path.exists():
        print("Build failed or cuda_bench_app not found", file=sys.stderr)
        sys.exit(1)
    if args.build:
        return

    use_xsched = not args.no_xsched
    xserver_proc = None


    try:
        high_times, low_times = run_bench(
            xsched_path=xsched_path,
            app_path=app_path,
            duration_sec=args.duration,
            num_low=args.num_low,
            use_xsched=use_xsched,
            iterations=args.iterations,
            high_kernel_ms=args.high_kernel_ms,
            low_kernel_ms=args.low_kernel_ms,
            high_interval_ms=args.high_interval_ms,
            low_interval_ms=args.low_interval_ms,
            stagger_sec=args.stagger_sec,
        )
    finally:
        if xserver_proc:
            xserver_proc.terminate()
            xserver_proc.wait()

    # Report
    mode = "XSched HPF-shmem" if use_xsched else "No XSched (baseline)"
    print(f"\n=== HPF-shmem Benchmark ({mode}) ===\n")
    print(f"High-priority processes: 1")
    print(f"Low-priority processes: {args.num_low}")
    print(
        f"Metric: kernel_ms = one launch + cudaStreamSynchronize (not sleep). "
        f"Intervals: high={args.high_interval_ms}ms, low={args.low_interval_ms}ms."
    )

    hs = stats(high_times)
    ls = stats(low_times)
    if hs:
        print(f"\n--- High-priority kernel latency (ms) ---")
        print(f"  count={hs['count']}, mean={hs['mean']:.2f}, p50={hs['p50']:.2f}, p99={hs['p99']:.2f}")
        print(f"  min={hs['min']:.2f}, max={hs['max']:.2f}")

    if ls:
        print(f"\n--- Low-priority kernel latency (ms) ---")
        print(f"  count={ls['count']}, mean={ls['mean']:.2f}, p50={ls['p50']:.2f}, p99={ls['p99']:.2f}")
        print(f"  min={ls['min']:.2f}, max={ls['max']:.2f}")


    print()


if __name__ == "__main__":
    main()
