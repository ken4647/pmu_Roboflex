#!/usr/bin/env python3
"""
从标准输入驱动守护进程中的全局事件位（与 server.c do_apply_event / do_cancel_event 一致）。

用法示例（需已启动 schedule_daemon）:
  apply obstacle_dense 0 1
  cancel obstacle_dense 1
  help
  quit
"""
from __future__ import annotations

import os
import sys

_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
if _SCRIPT_DIR not in sys.path:
    sys.path.insert(0, _SCRIPT_DIR)

from robflex_api import (  # noqa: E402
    SOCKET_PATH,
    robflex_apply_event,
    robflex_cancel_event,
    robflex_cleanup,
)

# 与 inc/robflex_event.h 中 robflex_event_name_list 顺序一致
EVENT_NAMES = (
    "obstacle_dense",
    "pedestrian_dense",
    "high_speed",
    "cruise_normal",
    "stationary",
    "interaction",
    "awaiting_command",
    "emergency_stop",
)


def _usage() -> None:
    print(f"Socket: {SOCKET_PATH}")
    print("合法事件名:", ", ".join(EVENT_NAMES))
    print("命令（每行一条，空格分隔）:")
    print("  apply <event_name> [timeout] [strength]   # 默认 timeout=0 strength=1")
    print("  cancel <event_name> [strength]            # 默认 strength=1")
    print("  help")
    print("  quit | exit")


def _parse_int(s: str, default: int) -> int:
    try:
        return int(s, 10)
    except ValueError:
        return default


def _handle_line(line: str) -> bool:
    """
    处理一行输入。返回 False 表示应退出主循环。
    """
    parts = line.split()
    if not parts:
        return True
    cmd = parts[0].lower()
    if cmd in ("quit", "exit", "q"):
        return False
    if cmd == "help" or cmd == "?":
        _usage()
        return True

    if cmd == "apply":
        if len(parts) < 2:
            print("用法: apply <event_name> [timeout] [strength]")
            return True
        name = parts[1]
        if len(parts) == 2:
            timeout, strength = 0, 1
        elif len(parts) == 3:
            timeout, strength = _parse_int(parts[2], 0), 1
        else:
            timeout = _parse_int(parts[2], 0)
            strength = _parse_int(parts[3], 1)
        ret = robflex_apply_event(name, timeout, strength)
        print(f"apply_event -> {ret}")
        return True

    if cmd == "cancel":
        if len(parts) < 2:
            print("用法: cancel <event_name> [strength]")
            return True
        name = parts[1]
        strength = _parse_int(parts[2], 1) if len(parts) > 2 else 1
        ret = robflex_cancel_event(name, strength)
        print(f"cancel_event -> {ret}")
        return True

    print("未知命令，输入 help 查看说明。")
    return True


def main() -> int:
    print("Roboflex 事件控制（stdin），守护进程套接字:", SOCKET_PATH)
    print("输入 help 查看命令，quit 退出。")
    try:
        for raw in sys.stdin:
            line = raw.strip()
            if line.startswith("#"):
                continue
            if not _handle_line(line):
                break
    except KeyboardInterrupt:
        print("\n中断退出。")
    finally:
        robflex_cleanup()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
