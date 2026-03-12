import socket
import os
import json
import threading
from typing import Optional, Any

# ================= 配置区域 =================
# 请确保这里的 SOCKET_PATH 与 C 语言代码中的宏定义一致
# 例如: "/tmp/robflex_daemon.sock"
SOCKET_PATH = "/tmp/schedule_daemon.sock"

# 模拟 C 代码中的宏大小限制 (可选，Python 动态字符串通常不需要，但为了逻辑一致保留)
MAX_JSON_SIZE = 4096 
MAX_LOG_SIZE = 1024

# ================= 内部实现 =================

# 使用 thread-local 存储 socket fd，对应 C 语言的 __thread int socket_fd
_local = threading.local()

def _get_socket() -> socket.socket:
    """获取当前线程的 socket 连接，如果没有则创建 (对应 C 中的懒加载逻辑)"""
    if not hasattr(_local, 'sock') or _local.sock is None:
        try:
            # AF_UNIX, SOCK_DGRAM 对应 C 代码中的 socket(AF_UNIX, SOCK_DGRAM, 0)
            _local.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
            # 设置超时时间，避免永久阻塞 (C 代码默认是阻塞的，Python 建议加个超时以防死锁)
            _local.sock.settimeout(5.0) 
        except OSError as e:
            raise RuntimeError(f"Failed to create socket: {e}")
    return _local.sock

def _send_command_to_daemon(cmd_dict: dict) -> int:
    """
    内部发送函数
    返回: 0 (成功), -1 (失败)
    """
    try:
        sock = _get_socket()
        
        # 序列化 JSON (对应 cJSON_PrintUnformatted)
        cmd_json = json.dumps(cmd_dict, separators=(',', ':'))
        
        if len(cmd_json) > MAX_JSON_SIZE:
            print(f"Warning: JSON payload too large ({len(cmd_json)} > {MAX_JSON_SIZE})")

        # 构建地址结构 (对应 struct sockaddr_un)
        # Python 的 sendto 对于 AF_UNIX 可以直接传路径字符串，或者 bytes 路径
        # 为了严格对应 C 的结构体行为，我们传入路径
        server_addr = SOCKET_PATH
        
        # 发送数据 (对应 sendto)
        # 注意：SOCK_DGRAM 是无连接的，每次 sendto 都需要指定地址
        num_bytes = sock.sendto(cmd_json.encode('utf-8'), server_addr)
        
        if num_bytes < 0:
            return -1
            
        return 0
        
    except Exception as e:
        # 对应 C 代码中注释掉的 perror，这里打印到 stderr 或日志
        # print(f"Send error: {e}") 
        return -1

# ================= 公开 API (对应 C 函数) =================

def robflex_set_scheduler(tid: int, policy: int, priority: int) -> int:
    """
    设置调度策略
    对应 C: int robflex_set_scheduler(pid_t tid, int policy, int priority)
    """
    if tid == 0:
        tid = os.gettid()
    
    payload = {
        "cmd": "sched_setscheduler",
        "tid": tid,
        "policy": policy,
        "priority": priority
    }
    
    return _send_command_to_daemon(payload)

def robflex_set_priority(tid: int, priority: int) -> int:
    """
    设置优先级
    对应 C: int robflex_set_priority(pid_t tid, int priority)
    """
    if tid == 0:
        tid = os.gettid()
        
    payload = {
        "cmd": "set_priority",
        "tid": tid,
        "priority": priority
    }
    
    return _send_command_to_daemon(payload)

def robflex_log_message(tid: int, message: str, *args: Any) -> int:
    """
    记录日志 (支持格式化字符串)
    对应 C: int robflex_log_message(pid_t tid, const char *message, ...)
    
    用法:
    robflex_log_message(0, "CPU usage is %d%%", 85)
    """
    # if tid == 0:
    #     tid = os.gettid()
    
    # 处理可变参数格式化 (对应 vsnprintf)
    if args:
        try:
            formatted_message = message % args
        except TypeError:
            # 如果用户传入的是 {} 格式而不是 % 格式，尝试 format
            formatted_message = message.format(*args)
    else:
        formatted_message = message

    if len(formatted_message) > MAX_LOG_SIZE:
        formatted_message = formatted_message[:MAX_LOG_SIZE]

    payload = {
        "cmd": "log_message",
        "tid": tid,
        "message": formatted_message
    }
    
    return _send_command_to_daemon(payload)

def robflex_update_ctrl_time_cost(tid: int, value_in_us: int) -> int:
    """
    更新控制时间成本
    对应 C: int robflex_update_ctrl_time_cost(pid_t tid, int value_in_us)
    """
    if tid == 0:
        tid = os.gettid()
        
    payload = {
        "cmd": "update_ctrl_time_cost",
        "tid": tid,
        "value_in_us": value_in_us
    }
    
    return _send_command_to_daemon(payload)

# ================= 清理函数 (可选) =================

def robflex_cleanup():
    """
    关闭当前线程的 socket。
    通常不需要手动调用，进程退出时会自动清理。
    如果在长生命周期程序中需要重置连接，可以调用此函数。
    """
    if hasattr(_local, 'sock') and _local.sock is not None:
        try:
            _local.sock.close()
        except:
            pass
        _local.sock = None

# ================= 使用示例 =================
if __name__ == "__main__":
    print(f"Connecting to daemon at: {SOCKET_PATH}")
    
    # 1. 测试日志
    ret = robflex_log_message(0, "Python test log: Hello from PID %d", 0)
    print(f"log_message result: {ret}")
    
    # # 2. 测试更新 CPI (假设值为 100 百万分之一)
    # ret = robflex_update_ctrl_time_cost(0, 100)
    # print(f"update_ctrl_time_cost result: {ret}")
    
    # # 3. 测试设置调度策略 (SCHED_FIFO = 1, Priority = 10)
    # # 注意：这需要守护进程有 CAP_SYS_NICE 权限才能实际生效
    # ret = robflex_set_scheduler(0, 1, 10)
    # print(f"set_scheduler result: {ret}")
    
    # # 4. 测试设置优先级
    # ret = robflex_set_priority(0, 20)
    # print(f"set_priority result: {ret}")
    
    # 清理
    robflex_cleanup()