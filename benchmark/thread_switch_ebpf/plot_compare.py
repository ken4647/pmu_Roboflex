import sys
import os
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

START_MS = 110   # 从第 100ms 开始记录，避免启动阶段缺乏普遍性
MAX_MS = 200     # 记录 200ms 的窗口
DRAW_INTERVALS = True  # 是否绘制 t0->t1->t2->... 之间的间隔（数字 + 双向箭头）

def build_segments(logs):
    """从 logs 构建 segments 和 ticks，从 START_MS 起记录 MAX_MS 内"""
    if not logs:
        return [], []
    start_ts = logs[0][0]
    segments = []
    ticks = []
    last_end_ms = 0.0  # 在 [0, MAX_MS] 窗口内的坐标
    active = None      # (start_ms_in_window, comm) 若任务在窗口前开始则 start 取 0
    for ts, tid, event, comm in sorted(logs, key=lambda x: (x[0], 1 if x[2]=='S' else 0)):
        ms_raw = (ts - start_ts) / 1e6
        in_window = START_MS <= ms_raw <= START_MS + MAX_MS
        ms = ms_raw - START_MS if in_window else 0
        if ms_raw > START_MS + MAX_MS:
            break
        if event == 'S':
            if ms_raw >= START_MS:
                if last_end_ms < ms:
                    seg = (last_end_ms, min(ms - last_end_ms, MAX_MS - last_end_ms), 'other')
                    if seg[1] > 0:
                        segments.append(seg)
                active = (ms, comm)
            else:
                active = (0, comm)  # 任务在窗口前开始，在窗口内从 0 计
        elif event == 'E' and active and active[1] == comm:
            if ms_raw >= START_MS:
                seg_start = max(active[0], 0)
                dur = min(ms - seg_start, MAX_MS - seg_start)
                if dur > 0:
                    segments.append((seg_start, dur, comm))
                last_end_ms = ms
            active = None
        elif event == 'TICK' and in_window:
            ticks.append(ms)
    return segments, ticks


def draw_timeline_merged(logs_rt, logs_pmu, ax):
    """在同一个 ax 内绘制 RT 与 PMU 两条时间线"""
    h = 3
    y_rt, y_pmu = 11, 2  # RT 在上，PMU 在下，间距拉大

    def draw_row(segments, y, x_right):
        for start, dur, task_type in segments:
            if start >= x_right:
                continue
            dur = min(dur, x_right - start)
            if dur <= 0:
                continue
            if task_type == 'other':
                rect = mpatches.Rectangle((start, y), dur, h, fill=False,
                    edgecolor='gray', linestyle='--', linewidth=1.5)
            elif 'cycle1' in task_type or task_type == 'taskA':
                rect = mpatches.Rectangle((start, y), dur, h, facecolor='white',
                    edgecolor='black', hatch='///', linewidth=0.8)
            else:
                rect = mpatches.Rectangle((start, y), dur, h, facecolor='white',
                    edgecolor='black', hatch='xxx', linewidth=0.8)
            ax.add_patch(rect)

    segs_rt, ticks_rt = build_segments(logs_rt)
    segs_pmu, ticks_pmu = build_segments(logs_pmu)
    data_max_rt = max((s[0]+s[1] for s in segs_rt), default=0)
    data_max_pmu = max((s[0]+s[1] for s in segs_pmu), default=0)
    x_right = min(MAX_MS, max(min(data_max_rt, data_max_pmu), 1))  # 取较短者，缩短对齐

    draw_row(segs_rt, y_rt, x_right)
    draw_row(segs_pmu, y_pmu, x_right)

    # 只显示 t < x_right 的 tick，避免末尾多余竖线
    ticks_rt_show = [x for x in sorted(ticks_rt) if x < x_right]
    ticks_pmu_show = [x for x in sorted(ticks_pmu) if x < x_right]

    line_h = h * 3
    for i, t in enumerate(ticks_rt_show):
        y_lo, y_hi = y_rt - line_h/4, y_rt + h + line_h/4
        ax.plot([t, t], [y_lo, y_hi], color='black', linestyle='--', alpha=0.6, linewidth=1.5)
        ax.text(t, y_lo - 0.5, f' t{i}', fontsize=20, ha='left')
    for i, t in enumerate(ticks_pmu_show):
        y_lo, y_hi = y_pmu - line_h/4, y_pmu + h + line_h/4
        ax.plot([t, t], [y_lo, y_hi], color='black', linestyle='--', alpha=0.6, linewidth=1.5)
        ax.text(t, y_lo - 0.5, f" t{i}'", fontsize=20, ha='left')

    def draw_intervals(ticks_show, y_lo):
        """在条形下方画出 t_i -> t_{i+1} 的间隔：数字 + 双向箭头"""
        for i in range(len(ticks_show) - 1):
            t0, t1 = ticks_show[i], ticks_show[i + 1]
            dt = t1 - t0
            mid = (t0 + t1) / 2
            y_arr = y_lo + 1.5
            ax.annotate('', xy=(t1, y_arr), xytext=(t0, y_arr),
                        arrowprops=dict(arrowstyle='<->', color='gray', lw=1.2))
            ax.text(mid, y_arr - 0.3, f'{dt:.1f}ms', fontsize=12, ha='center', va='top')

    if DRAW_INTERVALS:
        draw_intervals(ticks_rt_show, y_rt - line_h/4 - 0.5)
        draw_intervals(ticks_pmu_show, y_pmu - line_h/4 - 0.5)

    p_other = mpatches.Patch(facecolor='none', edgecolor='gray', linestyle='--', label='other task')
    p_a = mpatches.Patch(facecolor='white', edgecolor='black', hatch='///', label='task A')
    p_b = mpatches.Patch(facecolor='white', edgecolor='black', hatch='xxx', label='task B')
    ax.legend(handles=[p_a, p_b, p_other], loc='upper right', bbox_to_anchor=(1, 1.01), fontsize=16)

    ax.set_xlim(0, x_right)
    ax.set_ylim(-2, 20)
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['bottom'].set_visible(False)
    ax.set_xlabel('')
    ax.set_xticks([])
    ax.set_xticklabels([])
    ax.set_yticks([y_rt + h/2, y_pmu + h/2])
    ax.set_yticklabels(['RT', 'PMU'], fontsize=20)
    ax.grid(False)


def parse_log_line(line):
    """解析 event_log 格式: [ts] PID:x TID:y EVENT:b'S'/b'E' COMM:name 或 [ts] EXTERNAL_EVENT: ... tick"""
    line = line.strip()
    if not line:
        return None
    parts = line.split()
    if len(parts) < 2:
        return None
    if not parts[0].startswith('[') or ']' not in parts[0]:
        return None
    try:
        ts = int(parts[0].strip('[]'))
    except ValueError:
        return None
    if parts[1] == 'EXTERNAL_EVENT:':
        if 'tick' in ' '.join(parts[2:]).lower():
            return (ts, 0, 'TICK', '')
        return None
    if len(parts) < 5:
        return None
    tid = int(parts[2].split(':')[1])
    ev = parts[3]  # "EVENT:b'S'" or "EVENT:b'E'"
    event = ev.split("'")[1]
    comm = parts[4].split(':')[1]
    return (ts, tid, event, comm)


def load_logs(filepath):
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    # 跳过开头非事件行（如 "Started Task_A..."）
    raw_logs = []
    for line in lines:
        parsed = parse_log_line(line)
        if parsed is not None:
            raw_logs.append(parsed)
    return raw_logs


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <log1.txt> <log2.txt>", file=sys.stderr)
        print("Example: python3 plot_compare.py log/event_log_rt.txt log/event_log_pmu.txt  # 第1个=RT, 第2个=PMU", file=sys.stderr)
        sys.exit(1)
    file1, file2 = sys.argv[1], sys.argv[2]
    for f in (file1, file2):
        if not os.path.exists(f):
            print(f"Error: file not found: {f}", file=sys.stderr)
            sys.exit(1)

    logs_rt = load_logs(file1)
    logs_pmu = load_logs(file2)
    if not logs_rt or not logs_pmu:
        print("Error: no valid event logs in one or both files", file=sys.stderr)
        sys.exit(1)

    fig, ax = plt.subplots(figsize=(14, 6))
    draw_timeline_merged(logs_rt, logs_pmu, ax)
    plt.tight_layout()
    plt.savefig('timeline_compare.png')
    plt.close()
    print("Saved timeline_compare.png")