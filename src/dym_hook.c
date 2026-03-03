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


__thread int perf_fd = 0;
__thread volatile int interrupt_count = 0;
__thread volatile int timer_count = 0;
__thread struct timespec start_time;

// perf_event_open 系统调用包装
static int perf_event_open(struct perf_event_attr *attr, pid_t pid,
                           int cpu, int group_fd, unsigned long flags) {
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

pid_t gettid() {
    return syscall(__NR_gettid);
}

static inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


// 可以考虑换成ptrace来实现，先忽略函数是否为signal_safe
__thread uint64_t past, now, diff;
__thread uint64_t avg_timecost_ns = 0;
void instruction_interrupt_handler(int signo, siginfo_t *info, void *context) {
    interrupt_count++;

    const uint64_t target_time_ns = 10 * 1000 * 1000;  // 1ms = 1,000,000 纳秒

    now = get_time_ns();
    diff = now - past;

    // printf("diff: %lld\n", diff/(1000*1000));
    
    if (diff < target_time_ns) {
        // 休眠补足剩余时间
        struct timespec sleep_ts;
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = target_time_ns - diff;
        // printf("sleep: %ld us\n", sleep_ts.tv_nsec/1000);
        
        nanosleep(&sleep_ts, NULL);
    } 


    avg_timecost_ns = avg_timecost_ns * 0.9 + diff * 0.1;  // 简单的指数移动平均
    past = get_time_ns();

    // 重要：重置计数器，否则只会触发一次
    ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);
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
    attr.config = PERF_COUNT_HW_INSTRUCTIONS;

    // 关键配置：采样模式
    attr.sample_period = 50*1000*1000ULL;  // 每5000万条指令采样一次
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
    // robflex_set_scheduler(gettid(), SCHED_FIFO, 10);

    return 0;
}

int setup_robflex_if_enabled() {
    if (is_robflex_enabled()) {
        set_high_nice();
        setup_perf_ctrl();
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

static int (*real_clone)(int (*fn)(void *), void *child_stack,
                         int flags, void *arg, ...);

int clone(int (*fn)(void *), void *child_stack,
          int flags, void *arg, ...)
{
    if (!real_clone)
        real_clone = dlsym(RTLD_NEXT, "clone");

    int pid = real_clone(fn, child_stack, flags, arg);

    // 如果不是线程 clone，而是新进程
    if (!(flags & CLONE_THREAD)) {
        if (pid == 0) {
            setup_robflex_if_enabled();
        }
    }

    return pid;
}
