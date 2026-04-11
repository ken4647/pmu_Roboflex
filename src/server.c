// 基于Unix Socket实现调度策略设置并提供基础的日志服务
// 主要功能：
// 1. 创建Unix Socket服务器，监听来自客户端的连接请求
// 2. 接收客户端发送的调度策略设置命令，并执行相应的系统调用来设置调度策略
// 3. 使用json格式进行通信，方便扩展和解析
// 4. 策略修改通常为低频操作，只使用单线程+SOCK_DGRAM即可满足需求，不需要复杂的多线程处理
// 5. 需要实现的接口包括， Json字段完全根据Glibc函数字段设计即可：
//    - sched_setscheduler(tid, policy, priority) ，要求tid!=0，否则不知道是哪个线程需要设置
//    - setpriority(PRIO_PROCESS, tid, priority) ，特别要求tid!=0，否则不知道是哪个线程需要设置
//    - log_message(tid, message) 记录日志消息，要求tid!=0，表示哪个线程发送的日志
// 6. 需要检查CAP_SYS_NICE权限，确保有权限设置调度策略

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/capability.h>
#include <sys/resource.h>
#include <sched.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <linux/futex.h>
#include <limits.h>
#include <signal.h>

#include <cJSON.h>

#include <robflex_def.h>

#ifndef SCHED_IDLE
#define SCHED_IDLE 5
#endif

struct UnixSocketServer {
    int socket_fd;
    struct sockaddr_un addr;
};

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total;
    unsigned long long non_idle;
} CPUData;

#define CPU_STAT_INTERVAL_US 10000
#define IDLE_WAKE_WAIT_FOR_NS 5000
#define CPU_GAP_THRESHOLD 3000

static SystemData *ptr_shmem = NULL;
static volatile sig_atomic_t g_running = 1;
static pid_t g_xserver_pid = -1;

#define DEFAULT_XSERVER_PATH "third_party/xsched/output/bin/xserver"
#define DEFAULT_XSERVER_POLICY "HPF"
#define DEFAULT_XSERVER_PORT "50000"

static enum SystemBusyDegree get_system_busy_degree(float load_1s) {
    if (load_1s < 30.0f) {
        return SYSTEM_IDLE;
    } else if (load_1s < 80.0f) {
        return SYSTEM_MODERATE;
    } else {
        return SYSTEM_HIGH;
    }
}

static uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

uint64_t get_thread_cpu_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void read_cpu_data(FILE *fp, CPUData *cpus, int *count, int ncpu) {
    char buffer[1024];
    int i = 0;
    rewind(fp);

    while (fgets(buffer, sizeof(buffer), fp) && i < ncpu + 1) {
        if (strncmp(buffer, "cpu", 3) != 0) {
            break;
        }

        CPUData *c = &cpus[i];
        char name[16];
        if (sscanf(buffer, "%15s %llu %llu %llu %llu %llu %llu %llu %llu",
                   name, &c->user, &c->nice, &c->system, &c->idle,
                   &c->iowait, &c->irq, &c->softirq, &c->steal) < 8) {
            break;
        }
        c->non_idle = c->user + c->nice + c->system + c->irq + c->softirq + c->steal;
        c->total = c->non_idle + c->idle + c->iowait;
        i++;
    }

    *count = i;
}

static float read_system_load(const CPUData *cur, const CPUData *prev) {
    unsigned long long total_diff = cur[0].total - prev[0].total;
    unsigned long long idle_diff = (cur[0].idle + cur[0].iowait) - (prev[0].idle + prev[0].iowait);

    if (total_diff == 0) {
        return 0.0f;
    }

    return (float)(total_diff - idle_diff) / (float)total_diff * 100.0f;
}

static SystemData *get_shmem_data(const char *shmem_name) {
    if (ptr_shmem != NULL) {
        return ptr_shmem;
    }

    int fd = shm_open(shmem_name, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        perror("shm_open");
        return NULL;
    }

    if (ftruncate(fd, sizeof(SystemData)) == -1) {
        perror("ftruncate");
        close(fd);
        return NULL;
    }

    ptr_shmem = (SystemData *)mmap(NULL, sizeof(SystemData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr_shmem == MAP_FAILED) {
        perror("mmap");
        ptr_shmem = NULL;
    }
    close(fd);
    return ptr_shmem;
}

static void *cpu_monitor_thread(void *arg) {
    (void)arg;
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("fopen /proc/stat");
        return NULL;
    }

    int online_ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (online_ncpu <= 0) {
        fprintf(stderr, "invalid online cpu count\n");
        fclose(fp);
        return NULL;
    }

    CPUData prev[online_ncpu + 1];
    CPUData curr[online_ncpu + 1];
    int cpu_count = 0;
    float load_1s = 30.0f;

    read_cpu_data(fp, prev, &cpu_count, online_ncpu);
    while (1) {
        usleep(CPU_STAT_INTERVAL_US);
        uint64_t current_time = get_time_ns();
        read_cpu_data(fp, curr, &cpu_count, online_ncpu);

        float load_10ms = read_system_load(curr, prev);
        load_1s = load_1s * 0.9f + load_10ms * 0.1f;

        atomic_store(&ptr_shmem->load_10ms, load_10ms);
        atomic_store(&ptr_shmem->load_1s, load_1s);
        atomic_store(&ptr_shmem->last_update_time, current_time);
        atomic_store(&ptr_shmem->busy_degree, get_system_busy_degree(load_1s));

        prev[0] = curr[0];
    }

    fclose(fp);
    return NULL;
}

static int futex_wake_one(_Atomic int *uaddr) {
    return syscall(SYS_futex, (int *)uaddr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

static void *idle_waker_thread(void *arg) {
    (void)arg;
    if (sched_setscheduler(0, SCHED_IDLE, &(struct sched_param){.sched_priority = 0}) != 0) {
        perror("sched_setscheduler idle_waker_thread");
    }

    uint64_t lasting_cpu_time = 0;
    uint64_t last_cpu_time = get_thread_cpu_time_ns();
    while (1) {
        // yield cpu to other threads, if no other threads are running,
        //  this thread will run forward and get lasting_cpu_time updated.
        sched_yield();

        uint64_t now_cpu = get_thread_cpu_time_ns();
        uint64_t delta = now_cpu - last_cpu_time;
    
        if (delta < CPU_GAP_THRESHOLD) {
            lasting_cpu_time += delta;
        } else {
            lasting_cpu_time = 0;
        }

        last_cpu_time = now_cpu;

        if (lasting_cpu_time >= IDLE_WAKE_WAIT_FOR_NS) {
            futex_wake_one(&ptr_shmem->futex_wake_seq);
            // printf("waked up one thread\n");
            lasting_cpu_time = 0;
        }
    }
    return NULL;
}

int check_capability(cap_value_t cap) {
    cap_t caps = cap_get_proc();
    if (caps == NULL) {
        perror("cap_get_proc");
        return -1;
    }

    cap_flag_value_t has_cap;
    if (cap_get_flag(caps, cap, CAP_EFFECTIVE, &has_cap) == -1) {
        perror("cap_get_flag");
        cap_free(caps);
        return -1;
    }

    cap_free(caps);
    return has_cap == CAP_SET ? 0 : -1;
}

int setup_unix_socket(struct UnixSocketServer *server) {
    // 创建Unix Socket
    server->socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        perror("socket");
        return -1;
    }

    // 设置地址结构
    memset(&server->addr, 0, sizeof(struct sockaddr_un));
    server->addr.sun_family = AF_UNIX;
    strncpy(server->addr.sun_path, SOCKET_PATH, sizeof(server->addr.sun_path) - 1);

    // 绑定Socket
    if (bind(server->socket_fd, (struct sockaddr *)&server->addr, sizeof(struct sockaddr_un)) < 0) {
        perror("bind");
        close(server->socket_fd);
        return -1;
    }

    printf("Unix Socket Server is listening on /tmp/schedule_daemon.sock\n");
    return 0;
}

static void signal_handler(int signo) {
    (void)signo;
    g_running = 0;
}

static int setup_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0) {
        perror("sigaction SIGINT");
        return -1;
    }
    if (sigaction(SIGTERM, &sa, NULL) != 0) {
        perror("sigaction SIGTERM");
        return -1;
    }
    return 0;
}

static int start_xsched_xserver(const char *xserver_path, const char *policy, const char *port) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork xserver");
        return -1;
    }
    if (pid == 0) {
        execl(xserver_path, xserver_path, policy, port, (char *)NULL);
        perror("execl xserver");
        _exit(127);
    }
    g_xserver_pid = pid;
    printf("XSched xserver started: pid=%d, path=%s, policy=%s, port=%s\n",
           pid, xserver_path, policy, port);
    return 0;
}

static void stop_xsched_xserver(void) {
    if (g_xserver_pid <= 0) return;
    if (kill(g_xserver_pid, SIGTERM) != 0 && errno != ESRCH) {
        perror("kill xserver");
    }
    (void)waitpid(g_xserver_pid, NULL, 0);
    g_xserver_pid = -1;
}

int do_sched_setscheduler(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *policy = cJSON_GetObjectItem(root, "policy");
    cJSON *priority = cJSON_GetObjectItem(root, "priority");

    if (!tid || !policy || !priority) {
        fprintf(stderr, "Missing required fields for sched_setscheduler\n");
        return -1;
    }

    printf("sched_setscheduler: tid: %d, policy: %d, priority: %d\n", tid->valueint, policy->valueint, priority->valueint);

    // SCHED_OTHER时priority应解释为nice值
    if (policy->valueint == SCHED_OTHER) {
        // 依然需要调用sched_setscheduler，但priority字段无效
        struct sched_param param;
        param.sched_priority = 0; // SCHED_OTHER下忽略sched_priority
        if(sched_setscheduler(tid->valueint, SCHED_OTHER, &param) < 0) {
            return -1;
        }
        // set nice to "priority"
        if(setpriority(PRIO_PROCESS, tid->valueint, priority->valueint) < 0) {
            return -1;
        }

        return 0;
    } else {
        struct sched_param param;
        param.sched_priority = priority->valueint;
        return sched_setscheduler(tid->valueint, policy->valueint, &param);
    }
}

int do_set_priority(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *priority = cJSON_GetObjectItem(root, "priority");
    if (!tid || !priority) {
        fprintf(stderr, "Missing required fields for set_priority\n");
        return -1;
    }

    printf("set_priority: tid: %d, priority: %d\n", tid->valueint, priority->valueint);

    return setpriority(PRIO_PROCESS, tid->valueint, priority->valueint);
}

int do_log_message(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *message = cJSON_GetObjectItem(root, "message");
    if (!tid || !message) {
        fprintf(stderr, "Missing required fields for log_message\n");
        return -1;
    }
    // 记录日志消息，可以根据需要将日志写入文件或输出到控制台
    printf("Log from thread %d: %s\n", tid->valueint, message->valuestring);
    return 0;
}

int do_update_ctrl_time_cost(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *value_in_us = cJSON_GetObjectItem(root, "value_in_us");
    if (!value_in_us || !tid) {
        fprintf(stderr, "Missing required fields for update_ctrl_time_cost\n");
        return -1;
    }

    // 发送信号给对应线程，触发cpi_set_interrupt_handler更新全局原子变量ctrl_time_cost_ns
    printf("Updated tid:%d control time cost to %d ms\n", tid->valueint, value_in_us->valueint);

    union sigval val;
    val.sival_int = value_in_us->valueint; // 设置 CPI 值
    return sigqueue(tid->valueint, CPI_SET_SIGNAL, val);
}

int do_apply_event(const cJSON *root) {
    cJSON *event_name = cJSON_GetObjectItem(root, "event_name");
    cJSON *timeout = cJSON_GetObjectItem(root, "timeout");
    cJSON *strength = cJSON_GetObjectItem(root, "strength");
    if (!event_name || !strength) {
        fprintf(stderr, "Missing required fields for apply_event\n");
        return -1;
    }

    printf("apply_event: event_name: %s, timeout: %d, strength: %d\n", event_name->valuestring, timeout->valueint, strength->valueint);

    int event_idx = robflex_get_event_idx(event_name->valuestring);
    if(event_idx == -1){
        fprintf(stderr, "Invalid event name: %s\n", event_name->valuestring);
        return -1;
    }

    return set_event_bit(ptr_shmem, event_idx);
}

int do_cancel_event(const cJSON *root) {
    cJSON *event_name = cJSON_GetObjectItem(root, "event_name");
    cJSON *strength = cJSON_GetObjectItem(root, "strength");
    if (!event_name ) {
        fprintf(stderr, "Missing required fields for cancel_event\n");
        return -1;
    }

    printf("cancel_event: event_name: %s, strength: %d\n", event_name->valuestring, strength->valueint);

    int event_idx = robflex_get_event_idx(event_name->valuestring);
    if(event_idx == -1){
        fprintf(stderr, "Invalid event name: %s\n", event_name->valuestring);
        return -1;
    }
    return unset_event_bit(ptr_shmem, event_idx);
}

int cmd_dispatcher(const cJSON *root) {
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd) {
        fprintf(stderr, "Missing 'cmd' field\n");
        return -1;
    }

    if (strcmp(cmd->valuestring, "sched_setscheduler") == 0) {
        return do_sched_setscheduler(root);
    } else if (strcmp(cmd->valuestring, "set_priority") == 0) {
        return do_set_priority(root);
    } else if (strcmp(cmd->valuestring, "log_message") == 0) {
        return do_log_message(root);
    } else if (strcmp(cmd->valuestring, "update_ctrl_time_cost") == 0) {
        return do_update_ctrl_time_cost(root);
    } else if (strcmp(cmd->valuestring, "apply_event") == 0) {
        return do_apply_event(root);
    } else if (strcmp(cmd->valuestring, "cancel_event") == 0) {
        return do_cancel_event(root);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd->valuestring);
        return -1;
    }
}

void handle_client_request(struct UnixSocketServer *server) {
    char buffer[256];
    struct sockaddr_un client_addr;
    socklen_t client_addr_len = sizeof(struct sockaddr_un);

    // 接收客户端请求
    ssize_t num_bytes = recvfrom(server->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                 (struct sockaddr *)&client_addr, &client_addr_len);
    if (num_bytes < 0) {
        if (errno == EINTR) {
            return;
        }
        perror("recvfrom");
        return;
    }
    buffer[num_bytes] = '\0'; // 确保字符串以null结尾

    // 处理不同的命令类型
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        fprintf(stderr, "Error parsing JSON\n");
        return;
    }

    if(cmd_dispatcher(root) != 0) {
        perror("cmd_dispatcher");
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
    return;
}

int main(int argc, char **argv) {
    struct UnixSocketServer server;
    pthread_t monitor_tid;
    pthread_t idle_waker_tid;
    int enable_xsched_xserver = 0;
    const char *xserver_path = getenv("ROBFLEX_XSERVER_PATH");
    const char *xserver_policy = DEFAULT_XSERVER_POLICY;
    const char *xserver_port = DEFAULT_XSERVER_PORT;
    if (xserver_path == NULL || xserver_path[0] == '\0') {
        xserver_path = DEFAULT_XSERVER_PATH;
    }
    
    printf("Schedule Daemon is Starting...\n");

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--enable-xsched-xserver") == 0) {
            enable_xsched_xserver = 1;
        } else if (strcmp(argv[i], "--xserver-path") == 0 && i + 1 < argc) {
            xserver_path = argv[++i];
        } else if (strcmp(argv[i], "--xserver-policy") == 0 && i + 1 < argc) {
            xserver_policy = argv[++i];
        } else if (strcmp(argv[i], "--xserver-port") == 0 && i + 1 < argc) {
            xserver_port = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--enable-xsched-xserver] [--xserver-path PATH] "
                   "[--xserver-policy POLICY] [--xserver-port PORT]\n", argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            return 1;
        }
    }

    if (setup_signal_handlers() != 0) {
        return 1;
    }

    // 检查CAP_SYS_NICE权限
    if(check_capability(CAP_SYS_NICE) != 0) {
        fprintf(stderr, "Insufficient privileges to set scheduling policy\n");
        return 1;
    }

    // 清除sock addr文件
    unlink(SOCKET_PATH);

    // 设置Unix Socket服务器
    if (setup_unix_socket(&server) != 0) {
        fprintf(stderr, "Failed to set up Unix Socket Server\n");
        return 1;
    }

    if (enable_xsched_xserver) {
        if (start_xsched_xserver(xserver_path, xserver_policy, xserver_port) != 0) {
            close(server.socket_fd);
            return 1;
        }
    } else {
        printf("XSched xserver is disabled by default; pass --enable-xsched-xserver to start it\n");
    }
    
    if (get_shmem_data(SHMEM_NAME) == NULL) {
        fprintf(stderr, "Failed to init shared memory for system load\n");
        return 1;
    }
    for (int i = 0; i < MAX_EVENT_NUM/(sizeof(uint64_t)*8); i++) {
        atomic_store(&ptr_shmem->event_bits[i], 0);
    }
    atomic_store(&ptr_shmem->futex_wake_seq, 0);

    if (pthread_create(&monitor_tid, NULL, cpu_monitor_thread, NULL) != 0) {
        perror("pthread_create cpu_monitor_thread");
        return 1;
    }

    if (pthread_detach(monitor_tid) != 0) {
        perror("pthread_detach cpu_monitor_thread");
        return 1;
    }

    if (pthread_create(&idle_waker_tid, NULL, idle_waker_thread, NULL) != 0) {
        perror("pthread_create idle_waker_thread");
        return 1;
    }

    if (pthread_detach(idle_waker_tid) != 0) {
        perror("pthread_detach idle_waker_thread");
        return 1;
    }

    // 设置当前线程为FIFO策略
    if (sched_setscheduler(0, SCHED_FIFO, &(struct sched_param){.sched_priority = 10}) != 0) {
        perror("sched_setscheduler");
    }

    printf("Ready to receive requests:\n");
    while (g_running) {
        handle_client_request(&server);
    }

    close(server.socket_fd);
    unlink(SOCKET_PATH);
    stop_xsched_xserver();

    return 0;
}

