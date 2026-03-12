#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <linux/sched.h>
#include <pthread.h>
#include <robflex_def.h>
#include <robflex_api.h>


void* _init_shmem_data(const char *shmem_name);

__thread int perf_fd = 0;
__thread volatile int interrupt_count = 0;
__thread volatile int timer_count = 0;
__thread struct timespec start_time;
// process-wide granular control time cost, can be updated by env ROBFLEX_PERF_CTRL_CPI_ENV or API set_perf_ctrl_cpi_atomic
atomic_ullong ctrl_time_cost_ns = DEFUALT_TIME_COST_PER_INTERRUPT_NS; 
inline void set_perf_ctrl_cpi_atomic(int value_in_us) {
    atomic_store(&ctrl_time_cost_ns, max(1, value_in_us) * 1000);
}

inline unsigned long long get_perf_ctrl_cpi_atomic() {
    return atomic_load(&ctrl_time_cost_ns);
}

// perf_event_open 系统调用包装
static int perf_event_open(struct perf_event_attr *attr, pid_t pid,
                           int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

pid_t gettid() {
    return syscall(__NR_gettid);
}

uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


// 可以考虑换成ptrace来实现，先忽略函数是否为signal_safe
__thread uint64_t past, now, diff;
__thread uint64_t avg_timecost_ns = DEFUALT_TIME_COST_PER_INTERRUPT_NS;
void instruction_interrupt_handler(int signo, siginfo_t *info, void *context) {
    interrupt_count++;

    const uint64_t target_time_ns = get_perf_ctrl_cpi_atomic();  // 1ms = 1,000,000 纳秒
    enum SystemBusyDegree busy_degree = robflex_system_busy_degree();

    now = get_time_ns();
    diff = now - past;

    if (diff < target_time_ns && busy_degree == SYSTEM_HIGH) {
        // 休眠补足剩余时间
        struct timespec sleep_ts;
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = target_time_ns - diff;
        
        nanosleep(&sleep_ts, NULL);
    } else if (diff < target_time_ns && busy_degree == SYSTEM_MODERATE) {
        // 仅仅让出CPU
        sched_yield();
    } else {
        // 不休眠，继续执行直到底层调度策略让出CPU
    }

    avg_timecost_ns = avg_timecost_ns * 0.9 + diff * 0.1;  // 简单的指数移动平均
    past = get_time_ns();

    // 重要：重置计数器，否则只会触发一次
    ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);
}

// 处理用户态设置 CPI 目标值的信号处理器
void cpi_set_interrupt_handler(int signo, siginfo_t *info, void *context) {
    if (signo == CPI_SET_SIGNAL) {
        int new_cpi = info->si_int;  // 假设通过 si_int 传递新的 CPI 目标值
        set_perf_ctrl_cpi_atomic(new_cpi);
        robflex_log_message(gettid(), "Updated CPI control to %d million instructions", new_cpi);
    }
}

int setup_param_recver() {
    // 暂时只需要处理极低频的数据，所以同样用实时信号的方式来实现
    
    // CPI_SET_SIGNAL 用于接收用户态设置的 CPI 目标值, 通过 set_perf_ctrl_cpi_atomic 接口更新全局原子变量 ctrl_time_cost_ns
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cpi_set_interrupt_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);

    // 注册 CPI_SET_SIGNAL 的信号处理器
    if (sigaction(CPI_SET_SIGNAL, &sa, NULL) < 0) {
        perror("sigaction for CPI_SET_SIGNAL");
        return 1;
    }

    return 0;
}

int setup_perf_ctrl() {
    if (perf_fd > 0){
        ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(perf_fd);
        perf_fd = 0;
    }

    struct perf_event_attr attr;
    struct sigaction sa;
    
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;

    // 关键配置：采样模式
    attr.sample_period = get_instr_slice();  // 每5000万条指令采样一次
    attr.sample_type = PERF_SAMPLE_IP;
    attr.freq = 0;  // 0表示使用period，1表示使用freq
    
    // 启用中断
    attr.wakeup_events = 1;
    
    // 排除内核空间（可选）
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;

    perf_fd = perf_event_open(&attr, 0, -1, -1, 0);
    if (perf_fd < 0) {
        perror("perf_event_open");
        printf("可能需要root权限或设置/proc/sys/kernel/perf_event_paranoid\n");
        return 1;
    }
    
    // 设置信号处理器
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = instruction_interrupt_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    
    if (sigaction(PERF_SIGNAL, &sa, NULL) < 0) {
        perror("sigaction");
        close(perf_fd);
        return 1;
    }
    
    // 将perf_fd与信号关联
    if (fcntl(perf_fd, F_SETFL, O_ASYNC) < 0 ||
        fcntl(perf_fd, F_SETOWN, gettid()) < 0) {
        perror("fcntl");
        close(perf_fd);
        return 1;
    }

    // 3. 【关键步骤】指定使用该实时信号
    // 默认情况下，即使注册了其他信号，perf_fd 仍可能发送 SIGIO。
    // 必须显式告诉内核发送 PERF_SIGNAL。
    if (fcntl(perf_fd, F_SETSIG, PERF_SIGNAL) < 0) {
        perror("fcntl F_SETSIG");
        close(perf_fd);
        return 1;
    }
    
    past = get_time_ns();
    
    // // 启用计数器
    ioctl(perf_fd, PERF_EVENT_IOC_ENABLE, 0);
    
    return 0;
}

void perf_ctrl_cleanup() {
    if (perf_fd > 0) {
        robflex_log_message(gettid(), "perf_ctrl_cleanup, with avg_timecost_ns: %lld us, interrupt times: %d", avg_timecost_ns/1000, interrupt_count);

        ioctl(perf_fd, PERF_EVENT_IOC_DISABLE, 0);
        close(perf_fd);
        perf_fd = 0;
    }
    // if perf_fd is not open, do nothing, may be env ROBFLEX_ENABLE_FEATURES is not set
}

int set_high_nice() {
    robflex_set_scheduler(gettid(), SCHED_OTHER, -20);
    // robflex_set_scheduler(gettid(), SCHED_RR, 10);

    return 0;
}

int setup_robflex_if_enabled() {
    if (is_robflex_enabled()) {
        _init_shmem_data(SHMEM_NAME);
        set_high_nice();
        setup_perf_ctrl();
        setup_param_recver();
    }
}

__attribute__((constructor(101)))
void perf_ctrl_init() {
    setup_robflex_if_enabled();
}

__attribute__((destructor(101)))
void perf_ctrl_destroy() {
    perf_ctrl_cleanup();
}

// pthread_create 包装，确保新线程也能正确设置 perf 监控
static int (*real_pthread_create)(pthread_t *, const pthread_attr_t *,
                                  void *(*)(void *), void *);

struct wrapper_arg {
    void *(*start_routine)(void *);
    void *arg;
};

static void *thread_start_wrapper(void *arg)
{
    struct wrapper_arg *w = arg;


    setup_robflex_if_enabled();
    
    void *ret = w->start_routine(w->arg);

    perf_ctrl_cleanup();

    free(w);
    return ret;
}

int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg)
{
    if (!real_pthread_create)
        real_pthread_create = dlsym(RTLD_NEXT, "pthread_create");

    struct wrapper_arg *w = malloc(sizeof(*w));
    w->start_routine = start_routine;
    w->arg = arg;

    return real_pthread_create(thread, attr,
                                thread_start_wrapper,
                                w);
}

static pid_t (*real_fork)(void) = NULL;

pid_t fork(void)
{
    if (!real_fork)
        real_fork = dlsym(RTLD_NEXT, "fork");

    pid_t pid = real_fork();

    if (pid == 0) {
        setup_robflex_if_enabled();
    }

    return pid;
}

struct clone_wrapper_arg {
    int (*fn)(void *);
    void *arg;
};

static int clone_start_wrapper(void *arg)
{
    struct clone_wrapper_arg *w = arg;

    setup_robflex_if_enabled();

    int ret = w->fn(w->arg);

    perf_ctrl_cleanup();

    free(w);

    return ret;
}

static int (*real_clone)(int (*)(void *), void *, int, void *, ...) = NULL;

int clone(int (*fn)(void *), void *child_stack,
          int flags, void *arg, ...)
{
    if (!real_clone)
        real_clone = dlsym(RTLD_NEXT, "clone");

    if (!(flags & CLONE_THREAD)) {
        int pid =  real_clone(fn, child_stack, flags, arg);
        if (pid == 0) {
            setup_robflex_if_enabled();
        }
        return pid;        
    }

    struct clone_wrapper_arg *w = malloc(sizeof(*w));
    w->fn = fn;
    w->arg = arg;

    va_list ap;
    va_start(ap, arg);

    void *ptid = NULL;
    void *tls  = NULL;
    void *ctid = NULL;

    if (flags & CLONE_PARENT_SETTID)
        ptid = va_arg(ap, void *);

    if (flags & CLONE_SETTLS)
        tls = va_arg(ap, void *);

    if (flags & CLONE_CHILD_SETTID)
        ctid = va_arg(ap, void *);

    va_end(ap);

    return real_clone(
        clone_start_wrapper,
        child_stack,
        flags,
        w,
        ptid,
        tls,
        ctid
    );
}
