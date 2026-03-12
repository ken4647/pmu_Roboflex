import sys
import yaml
import subprocess
import time
import threading
import os
import socket
from ctypes import c_uint32 as u32
from bcc import BPF

# --- eBPF 程序：raw tracepoint (优先) 或 kprobe fallback ---
ebpf_code_raw_tp = """
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

struct data_t {
    u32 pid;
    u32 tid;
    u64 ts;
    char type;
    char comm[TASK_COMM_LEN];
};

BPF_PERF_OUTPUT(events);
BPF_HASH(target_pids, u32, u32);

RAW_TRACEPOINT_PROBE(sched_switch)
{
    // TP_PROTO(bool preempt, struct task_struct *prev, struct task_struct *next)
    u32 cpu = bpf_get_smp_processor_id();
    if (cpu != 0) return 0;  // 仅监控 CPU 0，与 task 绑定的核心一致，减少事件量
    struct task_struct *prev = (struct task_struct *)ctx->args[1];
    struct task_struct *next = (struct task_struct *)ctx->args[2];
    u64 now = bpf_ktime_get_ns();
    u32 next_tgid, prev_tgid;

    bpf_probe_read(&next_tgid, sizeof(next_tgid), &next->tgid);
    bpf_probe_read(&prev_tgid, sizeof(prev_tgid), &prev->tgid);

    if (target_pids.lookup(&next_tgid)) {
        struct data_t data = {};
        u32 tid;
        data.pid = next_tgid;
        bpf_probe_read(&tid, sizeof(tid), &next->pid);
        data.tid = tid;
        data.ts = now;
        data.type = 'S';
        bpf_probe_read(&data.comm, sizeof(data.comm), &next->comm);
        events.perf_submit(ctx, &data, sizeof(data));
    }
    if (target_pids.lookup(&prev_tgid)) {
        struct data_t data = {};
        u32 tid;
        data.pid = prev_tgid;
        bpf_probe_read(&tid, sizeof(tid), &prev->pid);
        data.tid = tid;
        data.ts = now;
        data.type = 'E';
        bpf_probe_read(&data.comm, sizeof(data.comm), &prev->comm);
        events.perf_submit(ctx, &data, sizeof(data));
    }
    return 0;
}
"""

ebpf_code_kprobe = """
#include <uapi/linux/ptrace.h>
#include <linux/sched.h>

struct data_t {
    u32 pid;
    u32 tid;
    u64 ts;
    char type;
    char comm[TASK_COMM_LEN];
};

BPF_PERF_OUTPUT(events);
BPF_HASH(target_pids, u32, u32);

int trace_sched_switch(struct pt_regs *ctx, struct task_struct *prev) {
    if (bpf_get_smp_processor_id() != 0) return 0;
    u64 now = bpf_ktime_get_ns();
    struct task_struct *next = (struct task_struct *)bpf_get_current_task();
    u32 next_tgid, prev_tgid;

    bpf_probe_read_kernel(&next_tgid, sizeof(next_tgid), &next->tgid);
    bpf_probe_read_kernel(&prev_tgid, sizeof(prev_tgid), &prev->tgid);

    if (target_pids.lookup(&prev_tgid)) {
        struct data_t data = {};
        data.pid = prev_tgid;
        bpf_probe_read_kernel(&data.tid, sizeof(data.tid), &prev->pid);
        data.ts = now;
        data.type = 'E';
        bpf_probe_read(&data.comm, sizeof(data.comm), &prev->comm);
        events.perf_submit(ctx, &data, sizeof(data));
    }

    if (target_pids.lookup(&next_tgid)) {
        struct data_t data = {};
        data.pid = next_tgid;
        bpf_probe_read_kernel(&data.tid, sizeof(data.tid), &next->pid);
        data.ts = now;
        data.type = 'S';
        bpf_get_current_comm(&data.comm, sizeof(data.comm));
        events.perf_submit(ctx, &data, sizeof(data));
    }
    return 0;
}
"""

# --- Unix Socket 服务端逻辑 ---
UDS_PATH = "/tmp/bcc_msg.sock"

def start_uds_server():
    if os.path.exists(UDS_PATH):
        os.remove(UDS_PATH)
    server = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    server.bind(UDS_PATH)
    while True:
        data, _ = server.recvfrom(1024)
        if data:
            ts = time.monotonic_ns()  # 与 bpf_ktime_get_ns() 同一时间基准
            print(f"[{ts}] EXTERNAL_EVENT: {data.decode().strip()}")

# --- 主逻辑 ---
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <config.yaml>", file=sys.stderr)
        sys.exit(1)
    config_path = sys.argv[1]
    with open(config_path, 'r') as f:
        config = yaml.safe_load(f)

    # 2. 初始化 BPF：优先 raw tracepoint（更稳定），否则用 kprobe
    use_raw_tp = BPF.support_raw_tracepoint()
    b = BPF(text=ebpf_code_raw_tp if use_raw_tp else ebpf_code_kprobe)
    if not use_raw_tp:
        b.attach_kprobe(
            event_re=r"^finish_task_switch$|^finish_task_switch\.isra\.\d+$",
            fn_name="trace_sched_switch",
        )
    target_pids_map = b.get_table("target_pids")

    # 3. 先启动 UDS 服务端，再启动子进程，避免消息丢失
    threading.Thread(target=start_uds_server, daemon=True).start()
    time.sleep(0.1)  # 等待 bind 完成

    # 4. 启动子进程并记录 PID（用 shell=False 确保 p.pid 是实际工作进程的 PID，而非 shell）
    # work_dir = os.path.dirname(os.path.abspath(config_path))
    procs = []
    for task in config['tasks']:
        cmd_list = task['cmd'].strip().split()
        p = subprocess.Popen(cmd_list, shell=False, cwd='.')
        target_pids_map[u32(p.pid)] = u32(1)
        procs.append(p)
        print(f"Started {task['name']} with PID: {p.pid}")

    # 5. 定义事件回调
    def print_event(cpu, data, size):
        event = b["events"].event(data)
        tag = "START" if event.type == ord('S') else "END"
        print(f"[{event.ts}] PID:{event.pid} TID:{event.tid} EVENT:{event.type} COMM:{event.comm.decode()}")

    b["events"].open_perf_buffer(print_event, page_cnt=1024)  # 增大 buffer 减少丢事件

    # 6. 运行指定时间
    print(f"Monitoring for {config['duration']} seconds...")
    end_time = time.time() + config['duration']
    try:
        while time.time() < end_time:
            b.perf_buffer_poll(timeout=100)
    finally:
        # 清理工作
        print("Stopping processes...")
        for p in procs:
            p.terminate()
        if os.path.exists(UDS_PATH):
            os.remove(UDS_PATH)

if __name__ == "__main__":
    main()