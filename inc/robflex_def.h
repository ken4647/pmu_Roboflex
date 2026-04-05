#ifndef ROBFLEX_DEF_H
#define ROBFLEX_DEF_H

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <robflex_event.h>

#include "khash.h"

#define PERF_SIGNAL (SIGRTMIN+1)
#define CPI_SET_SIGNAL (SIGRTMIN+2)

#define ROBFLEX_ENABLE_ENV "ROBFLEX_ENABLE_FEATURES"
#define ROBFLEX_RUNMODE_ENV "ROBFLEX_RUNMODE"
#define ROBFLEX_PERF_CTRL_CPI_ENV "ROBFLEX_PERIOD_TIME_IN_US" // time
#define ROBFLEX_INSTRUCTION_SLICE_ENV "ROBFLEX_CYCLES_NUM" // cycles
#define ROBFLEX_CONFIG_PATH_ENV "ROBFLEX_CONFIG_PATH"

#define DEFUALT_PERIOD_CYCLES_NUM (1ULL*1000*1000) // cycles
#define DEFUALT_PERIOD_TIME_IN_NS (1ULL*1000*1000) // time

#define SOCKET_PATH "/tmp/schedule_daemon.sock"
#define MAX_JSON_SIZE (1024)
#define MAX_LOG_SIZE (512)
#define MAX_EVENT_NUM (1024)

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
    /** Protects concurrent access to the fields below (init with pthread_mutex_init). */
    pthread_mutex_t mtx_all;

    _Atomic float load_10ms;
    _Atomic float load_1s;
    _Atomic uint64_t last_update_time;
    _Atomic enum SystemBusyDegree busy_degree;

    // futex wait
    _Atomic int futex_wake_seq;

    // event bits
    _Atomic uint64_t event_bits[MAX_EVENT_NUM/(sizeof(uint64_t)*8)];
}__attribute__((aligned(8))) SystemData;

/** Set/clear one bit in @a sd->event_bits. @a event_idx in [0, MAX_EVENT_NUM). */
static inline int set_event_bit(SystemData *sd, int event_idx)
{
    if (sd == NULL || event_idx < 0 || event_idx >= MAX_EVENT_NUM) {
        return -1;
    }
    const unsigned word_bits = (unsigned)(sizeof(uint64_t) * 8);
    unsigned wi = (unsigned)event_idx / word_bits;
    unsigned bi = (unsigned)event_idx % word_bits;
    uint64_t mask = (uint64_t)1u << bi;
    (void)atomic_fetch_or_explicit(&sd->event_bits[wi], mask, memory_order_release);
    return 0;
}

static inline int unset_event_bit(SystemData *sd, int event_idx)
{
    if (sd == NULL || event_idx < 0 || event_idx >= MAX_EVENT_NUM) {
        return -1;
    }
    const unsigned word_bits = (unsigned)(sizeof(uint64_t) * 8);
    unsigned wi = (unsigned)event_idx / word_bits;
    unsigned bi = (unsigned)event_idx % word_bits;
    uint64_t mask = (uint64_t)1u << bi;
    (void)atomic_fetch_and_explicit(&sd->event_bits[wi], ~mask, memory_order_release);
    return 0;
}

static inline bool test_event_bit(SystemData *sd, int event_idx)
{
    if (sd == NULL || event_idx < 0 || event_idx >= MAX_EVENT_NUM) {
        return false;
    }
    const unsigned word_bits = (unsigned)(sizeof(uint64_t) * 8);
    unsigned wi = (unsigned)event_idx / word_bits;
    unsigned bi = (unsigned)event_idx % word_bits;
    uint64_t word = atomic_load_explicit(&sd->event_bits[wi], memory_order_acquire);
    return (word & ((uint64_t)1u << bi)) != 0;
}

enum RunPolicy {
    YIELDING = 0,   // sleep/yield when tick occurs (depend on System Status)
    PREDETERMINED,  // run as expected, whatever system is busy or idle
    IMMEDIATE,      // run as fast as possible, won't stop for signal handle
    LATENCY_ORIENTED,   // run to meet the latency meeting
    BY_HANDLER,     // user defined
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

struct HandlerDemand {
    void (*handler)(void);
    uint64_t saved_arg[3];
};

union uAuxData {
    struct YieldDemand norm;
    struct LatencyDemand lat;
    struct HandlerDemand hdf;
};

typedef struct LocalContext{
    enum RunPolicy policy;
    union uAuxData aux;

    // the order level for ctx usage
    int level;

    // 统计数据
    uint64_t cycles_num;
    uint64_t avg_timecost_ns;
}__attribute__((aligned(8))) LocalContext;


KHASH_MAP_INIT_INT(EventMap, LocalContext) // event_idx:int -> LocalContext

#endif // ROBFLEX_DEF_H