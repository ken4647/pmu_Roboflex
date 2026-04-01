#ifndef ROBFLEX_DEF_H
#define ROBFLEX_DEF_H

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdint.h>
#include <stdatomic.h>

#define PERF_SIGNAL (SIGRTMIN+1)
#define CPI_SET_SIGNAL (SIGRTMIN+2)

#define ROBFLEX_ENABLE_ENV "ROBFLEX_ENABLE_FEATURES"
#define ROBFLEX_RUNMODE_ENV "ROBFLEX_RUNMODE"
#define ROBFLEX_PERF_CTRL_CPI_ENV "ROBFLEX_PERIOD_TIME_IN_US" // time
#define ROBFLEX_INSTRUCTION_SLICE_ENV "ROBFLEX_CYCLES_NUM" // cycles

#define DEFUALT_PERIOD_CYCLES_NUM (1ULL*1000*1000) // cycles
#define DEFUALT_PERIOD_TIME_IN_NS (1ULL*1000*1000) // time

#define SOCKET_PATH "/tmp/schedule_daemon.sock"
#define MAX_JSON_SIZE (1024)
#define MAX_LOG_SIZE (512)

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#define SHMEM_NAME "pmu_rbnx_shmem"

enum SystemBusyDegree {
    SYSTEM_IDLE = 0,
    SYSTEM_MODERATE = 1,
    SYSTEM_HIGH = 2
};

typedef struct SystemData{
    _Atomic float load_10ms;
    _Atomic float load_1s;
    _Atomic uint64_t last_update_time;
    _Atomic enum SystemBusyDegree busy_degree; 
}__attribute__((aligned(8))) SystemData;

enum RunMode {
    YIELDING = 0,   // sleep/yield when tick occurs (depend on System Status)
    PREDETERMINED,  // run as expected, whatever system is busy or idle
    IMMEDIATE,      // run as fast as possible, won't stop for signal handle
    LATENCY_ORIENTED,   // run to meet the latency meeting
};

struct YieldDemand {
    atomic_ullong time_slice_ns;
    long long time_budgets;
    
};

struct LatencyDemand {
    uint64_t target_lat;
    uint64_t start_time;
    uint64_t hist_ncycle;
    uint64_t used_ncycle;
};

union uAuxData {
    struct YieldDemand norm;
    struct LatencyDemand lat;
};

typedef struct LocalContext{
    atomic_int in_critical; 
    atomic_int n_signal_pendings;
    enum RunMode run_mode;
    union uAuxData aux;
    uint64_t cycles_num;
    // 统计数据
    uint64_t avg_timecost_ns;
}__attribute__((aligned(8))) LocalContext;

#endif // ROBFLEX_DEF_H