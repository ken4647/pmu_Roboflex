import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

def draw_timeline(logs):
    fig, ax = plt.subplots(figsize=(14, 4))
    
    start_ts = logs[0][0]
    segments = []
    ticks = []
    
    last_end_ms = 0.0
    active = None
    for ts, tid, event, comm in sorted(logs, key=lambda x: (x[0], 1 if x[2]=='S' else 0)):
        ms = (ts - start_ts) / 1e6
        if event == 'S':
            if last_end_ms < ms:
                segments.append((last_end_ms, ms - last_end_ms, 'other'))
            active = (ms, comm)
        elif event == 'E' and active and active[1] == comm:
            segments.append((active[0], ms - active[0], comm))
            last_end_ms = ms
            active = None
        elif event == 'TICK':
            ticks.append(ms)

    y, h = 2, 3  # 矩形高度缩窄 50%
    for start, dur, task_type in segments:
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

    for i, t in enumerate(ticks):
        ax.axvline(x=t, color='black', linestyle='--', alpha=0.5)
        ax.text(t, -2, f't{i}', fontsize=20, ha='left')

    p_other = mpatches.Patch(facecolor='none', edgecolor='gray', linestyle='--', label='other task')
    p_a = mpatches.Patch(facecolor='white', edgecolor='black', hatch='///', label='task A')
    p_b = mpatches.Patch(facecolor='white', edgecolor='black', hatch='xxx', label='task B')
    ax.legend(handles=[p_other, p_a, p_b], loc='upper right', fontsize=20)

    ax.set_xlim(0, max((s[0]+s[1] for s in segments), default=1) * 1.02)
    ax.set_ylim(-5, 10)
    ax.set_xlabel('Time (ms) since Start', fontsize=20)
    ax.set_yticks([y + h/2])
    ax.set_yticklabels(['CPU'], fontsize=20)
    ax.set_ylabel('', fontsize=20)
    ax.tick_params(axis='x', labelsize=20)
    ax.grid(True, axis='x', linestyle=':', alpha=0.7)
    ax.set_title('CPU Thread Scheduling Timeline', fontsize=20)
    
    plt.tight_layout()
    plt.savefig('timeline.png')
    plt.close()


def parse_log_line(line):
    """解析 event_log 格式: [ts] PID:x TID:y EVENT:b'S'/b'E' COMM:name 或 [ts] EXTERNAL_EVENT: ... tick"""
    line = line.strip()
    if not line:
        return None
    parts = line.split()
    if len(parts) < 2:
        return None
    ts = int(parts[0].strip('[]'))
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


if __name__ == '__main__':
    with open('event_log.txt', 'r') as f:
        lines = f.readlines()[10:32]
    raw_logs = [parse_log_line(l) for l in lines]
    raw_logs = [x for x in raw_logs if x is not None]
    draw_timeline(raw_logs)