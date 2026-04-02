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
#include <linux/futex.h>
#include <pthread.h>
#include <robflex_def.h>
#include <robflex_api.h>
#include <stdatomic.h>
#include <errno.h>


void* _init_shmem_data(const char *shmem_name);
__thread SystemData *g_shmem_data = NULL;

__thread int perf_fd = 0;
// 统计信息
__thread volatile int interrupt_count = 0;
__thread uint64_t sleeped_time = 0;
__thread uint64_t avg_timecost_ns = DEFUALT_PERIOD_TIME_IN_NS;

// 异步信号处理函数，负责主动控制程序的吞吐
__thread LocalContext loc_ctx = {0};


inline void set_perf_ctrl_cpi_atomic(int value_in_us) {
    atomic_store(&loc_ctx.aux.norm.time_slice_ns, max(1, value_in_us) * 1000);
}

inline unsigned long long get_perf_ctrl_cpi_atomic() {
    return atomic_load(&loc_ctx.aux.norm.time_slice_ns);
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

__thread uint64_t past, now, diff;
static int futex_wait_timeout(_Atomic int *uaddr, int expected, uint64_t timeout_ns) {
    struct timespec ts;
    ts.tv_sec = timeout_ns / 1000000000ULL;
    ts.tv_nsec = timeout_ns % 1000000000ULL;
    return syscall(SYS_futex, (int *)uaddr, FUTEX_WAIT, expected, &ts, NULL, 0);
}

void handle_tick_vsleep() {
    const uint64_t target_time_ns = get_perf_ctrl_cpi_atomic();
    struct YieldDemand *yd = &loc_ctx.aux.norm;
    long long time_budgets = yd->time_budgets;

    now = get_time_ns();
    diff = now - past;
    time_budgets += (long long)target_time_ns - (long long)diff;

    if (time_budgets > 0) {
        uint64_t sleep_ns = min(time_budgets, target_time_ns);
        sleeped_time += sleep_ns;
        if (g_shmem_data != NULL) {
            int ret = futex_wait_timeout(&g_shmem_data->futex_wake_seq, 1, sleep_ns); // will only be timeout or wake up
            if (ret == -1 && errno != EAGAIN && errno != EINTR && errno != ETIMEDOUT) {
                // ignore unexpected wait errors, keep throttling loop robust
            }
        }
    }

    past = get_time_ns();
    uint64_t yield_time = past - now;
    time_budgets -= (long long)yield_time;
    time_budgets = min((long long)target_time_ns * 5, time_budgets);
    time_budgets = max(-(long long)target_time_ns * 5, time_budgets);
    avg_timecost_ns = avg_timecost_ns * 0.9 + diff * 0.1;
    yd->time_budgets = time_budgets;
}

void handle_tick_predetermined() {
    const uint64_t target_time_ns = get_perf_ctrl_cpi_atomic();  // 1ms = 1,000,000 纳秒
    enum SystemBusyDegree busy_degree = robflex_system_busy_degree();
    struct YieldDemand* yd = &loc_ctx.aux.norm;

    long long time_budgets = yd->time_budgets;

    now = get_time_ns();
    diff = now - past;

    time_budgets += (long long)target_time_ns - (long long)diff;
    if (time_budgets>0){
        uint64_t sleep_ns = min(time_budgets, target_time_ns);

        // 休眠补足剩余时间
        struct timespec sleep_ts;
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = sleep_ns;

        sleeped_time += sleep_ns;
        nanosleep(&sleep_ts, NULL);
    }

    past = get_time_ns();
    
    uint64_t yield_time = past - now;
    time_budgets -= (long long)yield_time;

    time_budgets = min((long long)target_time_ns*5, time_budgets);
    time_budgets = max(-(long long)target_time_ns*5, time_budgets);

    avg_timecost_ns = avg_timecost_ns * 0.9 + diff * 0.1;  // 简单的指数移动平均
    yd->time_budgets = time_budgets;
}

void handle_tick_yielding() {
    const uint64_t target_time_ns = get_perf_ctrl_cpi_atomic();  // 1ms = 1,000,000 纳秒
    enum SystemBusyDegree busy_degree = robflex_system_busy_degree();
    struct YieldDemand* yd = &loc_ctx.aux.norm;

    long long time_budgets = yd->time_budgets;

    now = get_time_ns();
    diff = now - past;
    time_budgets += (long long)target_time_ns - (long long)diff;
    // robflex_log_message(gettid(), "target_time_ns:%llu, diff:%llu, sub:%lld, time_budgets:%lld", target_time_ns, diff, (long long)target_time_ns - (long long)diff, time_budgets);
    if (time_budgets>0){
        if (busy_degree == SYSTEM_HIGH || busy_degree == SYSTEM_MODERATE) {
            // 仅仅让出CPU
            sched_yield();
        } else { //idle
            time_budgets = max(0, time_budgets+(long long)avg_timecost_ns-target_time_ns);

            // 不休眠，继续执行直到底层调度策略让出CPU
        }        
    }
    
    past = get_time_ns();
    
    uint64_t yield_time = past - now;
    time_budgets -= (long long)yield_time;

    time_budgets = min((long long)target_time_ns*5, time_budgets);
    time_budgets = max(-(long long)target_time_ns*5, time_budgets);

    avg_timecost_ns = avg_timecost_ns * 0.9 + diff * 0.1;  // 简单的指数移动平均
    yd->time_budgets = time_budgets;
    
}

void handle_tick_latency_oriented() {
    enum SystemBusyDegree busy_degree = robflex_system_busy_degree();
    now = get_time_ns();

    struct LatencyDemand* ld = &loc_ctx.aux.lat;
    uint64_t hist_ncycle = ld->hist_ncycle;
    uint64_t used_ncycle = ld->used_ncycle;
    uint64_t target_lat = ld->target_lat;
    uint64_t start_time = ld->start_time;
    uint64_t now = get_time_ns();
    uint64_t cycle_slice = get_instr_slice();
    uint64_t pass_ns = now - past;

    long long diff_ncycle = (long long)hist_ncycle - (long long)used_ncycle + cycle_slice;
    long long diff_time = (long long)now - (long long)start_time;
    long long time_budgets = (long long)target_lat - diff_time;

    ld->used_ncycle += cycle_slice;
    if(time_budgets <= 1000000 || busy_degree == SYSTEM_IDLE) { // only x ms left, so full run
        past = get_time_ns();
        return;
    }else if(busy_degree == SYSTEM_MODERATE){ // moderate busy, just yield
        sched_yield();
        past = get_time_ns();
        return;
    }

    long long period_assume = 0;
    if(diff_ncycle > 0) { // need to wait for next cycle
        period_assume = time_budgets*cycle_slice/diff_ncycle;
        // printf("period_assume: %llu, pass_ns: %llu, sleep_ns: %lld, time_budgets: %lld, hist_ncycle: %llu, cycle_slice: %llu\n", period_assume, pass_ns, period_assume - (long long)pass_ns - 1000000, time_budgets, hist_ncycle, cycle_slice);
        long long sleep_ns = period_assume - (long long)pass_ns - 1000000;
        if(sleep_ns > 1000000) {
            struct timespec sleep_ts;
            sleep_ts.tv_sec = 0;
            sleep_ts.tv_nsec = sleep_ns;
            nanosleep(&sleep_ts, NULL);
        }
    }
    

    past = get_time_ns();
    return;
}

void handle_tick_immediate() {
    return;
}

void handle_tick() {
    interrupt_count++;

    switch (loc_ctx.run_mode) {
        case PREDETERMINED:
            handle_tick_vsleep();
            break;
        case YIELDING:
            handle_tick_yielding();
            break;
        case LATENCY_ORIENTED:
            handle_tick_latency_oriented();
            break;
        case IMMEDIATE:
            handle_tick_immediate();
            break;
        default:
            break;
    }

    // 重要：重置计数器，否则只会触发一次
    ioctl(perf_fd, PERF_EVENT_IOC_RESET, 0);
}

void instruction_interrupt_handler(int signo, siginfo_t *info, void *context) {
    if (atomic_load_explicit(&loc_ctx.in_critical, memory_order_relaxed)) {
        // handle the tick later
        atomic_fetch_add(&loc_ctx.n_signal_pendings, 1);
        return;
    }

    handle_tick();
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

    // reset local context
    robflex_init_local_context(get_runmode_env());

    struct perf_event_attr attr;
    struct sigaction sa;
    
    memset(&attr, 0, sizeof(attr));
    attr.type = PERF_TYPE_HARDWARE;
    attr.size = sizeof(attr);
    attr.config = PERF_COUNT_HW_CPU_CYCLES;

    // 关键配置：采样模式
    attr.sample_period = get_instr_slice();  // 每5000万条指令采样一次
    attr.sample_type = PERF_SAMPLE_TID;
    attr.freq = 0;  // 0表示使用period，1表示使用freq
    attr.remove_on_exec = 1;
    
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
    pid_t tid = gettid();
    // robflex_log_message(tid, "assign perf signal to %d", tid);
    struct f_owner_ex owner;
    owner.type = F_OWNER_TID; // 明确指出我要绑定的是一个具体的线程 (TID)
    owner.pid = tid;

    if (fcntl(perf_fd, F_SETFL, O_ASYNC) < 0) {
        perror("fcntl O_ASYNC");
        return 1;
    }

    if (fcntl(perf_fd, F_SETOWN_EX, &owner) < 0) {
        perror("fcntl F_SETOWN_EX");
        return 1;
    }

    // 指定使用该实时信号
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
        robflex_log_message(gettid(), "target_ns:%llu, instr_cycles:%llu", get_perf_ctrl_cpi_atomic(), get_instr_slice());
        robflex_log_message(gettid(), "perf_ctrl_cleanup, with avg_timecost_ns: %lld us, interrupt times: %d, sleeped time: %llu", avg_timecost_ns/1000, interrupt_count, sleeped_time);

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
        g_shmem_data = _init_shmem_data(SHMEM_NAME);
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
