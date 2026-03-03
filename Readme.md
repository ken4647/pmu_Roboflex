# 基于 PMU 的用户态调度与调度守护进程

## 概要

1. **用户态调度（`robonix.so`）**  
   通过 PMU 支持下的 `perf_event_open()`，对每个线程配置指令计数采样事件，并使用实时信号（`PERF_SIGNAL = SIGRTMIN+1`）模拟“中断”。  
   在信号处理函数中根据前后两次采样的时间差，主动调用 `nanosleep()` 让出 CPU，从而实现一种粗粒度的“时间片”控制。

2. **调度守护进程（`robonix_daemon`）**  
   基于 Unix Domain Socket + JSON 协议，实现集中化的调度策略设置与简单日志服务。  
   应用通过 `robflex_api` 提供的 API 把请求发给守护进程，由守护进程完成：

   - `sched_setscheduler(tid, policy, priority)`
   - `setpriority(PRIO_PROCESS, tid, priority)`
   - `log_message(tid, message)`

## 项目结构

```txt
.
├── inc
│   ├── robflex_api.h      # 对外暴露的 API
│   └── robflex_def.h      # 公共宏定义（PERF_SIGNAL、SOCKET_PATH 等）
├── src
│   ├── dym_hook.c         # 通过 LD_PRELOAD hook pthread_create/fork/clone + PMU 配置
│   ├── robflex_api.c      # 客户端 API，实现与守护进程的 JSON/Unix Socket 通信
│   └── server.c           # 调度守护进程实现
├── Makefile               # 构建 robonix_daemon 和 robonix.so
└── Readme.md
```

- `src/dym_hook.c + src/robflex_api.c` => `robonix.so`  
- `src/server.c` => `robonix_daemon`

## 构建

### 依赖

- GCC / Clang 等 C 编译器
- `libcjson` 开发包（例如 Debian/Ubuntu: `sudo apt install libcjson-dev`）
- `libcap` 开发包（例如 Debian/Ubuntu: `sudo apt install libcap-dev`）
- POSIX 线程库、`libdl`（通常随 glibc 自带）

### 编译（使用 CMake）

在项目根目录创建并进入构建目录，然后配置与编译：

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

生成：

- `robonix_daemon`：调度守护进程二进制
- `librobonix.so`：用于 LD_PRELOAD 的共享库（包含 PMU hook + robflex API）

清理（从 `build` 目录直接删除生成文件即可，例如）：

```bash
rm -rf build
```

## 运行方式

### 1. 启动调度守护进程

守护进程需要具备 `CAP_SYS_NICE` 能力才能调用 `sched_setscheduler()`、`setpriority()` 等接口。

常见做法（任选其一）：

```bash
# 方式一：以 root 直接运行
sudo ./robonix_daemon

# 方式二：给二进制授予 CAP_SYS_NICE 能力
sudo setcap 'cap_sys_nice=eip' ./robonix_daemon
./robonix_daemon
```

守护进程会在 `/tmp/schedule_daemon.sock` 上创建 Unix DGRAM socket，并循环处理来自客户端的 JSON 命令。

### 2. 在目标进程中启用 PMU 用户态调度

把 `robonix.so` 以 `LD_PRELOAD` 的方式注入目标进程：

```bash
LD_PRELOAD=$PWD/robonix.so your_app_binary ...
```

效果：

- 在进程主线程以及通过 `pthread_create` / `fork` / `clone` 创建的线程/子进程中，自动调用 `setup_perf_for_thread()`：
  - 调用 `perf_event_open()` 订阅硬件指令计数事件
  - 注册实时信号处理函数 `instruction_interrupt_handler()`
  - 当指令数达到设定门限时触发信号，在 handler 中根据运行时间决定是否 `nanosleep()`，实现简单的“时间片”控制。

## 守护进程通信协议

所有命令通过 Unix DGRAM socket（`/tmp/schedule_daemon.sock`）发送，负载为一条 JSON 文本。

### 1. 设置线程调度策略：`sched_setscheduler`

请求示例：

```json
{
  "cmd": "sched_setscheduler",
  "tid": 12345,
  "policy": 2,
  "priority": 10
}
```

- `tid`：线程 ID（Linux TID，必须为非 0）
- `policy`：调度策略，对应 glibc 中的 `SCHED_OTHER` / `SCHED_FIFO` / `SCHED_RR` 等整数值
- `priority`：实时优先级

### 2. 调整 nice 值：`set_priority`

请求示例：

```json
{
  "cmd": "set_priority",
  "tid": 12345,
  "priority": -5
}
```

- 对应 `setpriority(PRIO_PROCESS, tid, priority)`，`tid` 同样是线程 ID。

### 3. 记录日志：`log_message`

请求示例：

```json
{
  "cmd": "log_message",
  "tid": 12345,
  "message": "worker loop started"
}
```

当前实现简单地打印到守护进程的标准输出，你可以按需扩展为写入文件或接入 `log.c` 日志库。

## C 侧 API 使用（`robflex_api`）

在应用中包含头文件：

```c
#include <robflex_api.h>
```

### 1. 设置调度策略

```c
pid_t tid = syscall(SYS_gettid);  // 或其它方式获取线程 tid
robflex_set_scheduler(tid, SCHED_FIFO, 10);
```

### 2. 调整优先级（nice）

```c
pid_t tid = syscall(SYS_gettid);
robflex_set_priority(tid, -5);
```

### 3. 发送日志

```c
pid_t tid = syscall(SYS_gettid);
robflex_log_message(tid, "worker %d started with priority %d", tid, -5);
```

底层会自动构造 JSON，并通过 Unix DGRAM socket 发送到正在运行的 `robonix_daemon`。

## 后续可扩展点

- 将 `do_log_message()` 接入 `src/log.c`，统一日志格式与输出（文件/滚动日志等）。
- 在 JSON 协议中增加返回码与响应消息，实现简单的 request/response 协议。
- 更精细的调度策略（例如按 CPU 使用率、延迟等动态调整采样 period 和时间片长度）。  

