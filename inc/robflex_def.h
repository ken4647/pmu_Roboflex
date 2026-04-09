#ifndef ROBFLEX_DEF_H
#define ROBFLEX_DEF_H

#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <robflex_event.h>
#ifdef ROBFLEX_USE_KHASH_EVENT_MAP
#include "khash.h"
#endif

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

struct ChainContext {
    uint64_t total_latency;
    uint64_t used_latency;
    uint64_t local_target_latency;
    uint64_t local_used_latency;
};

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

    long long lat_bias;

    uint64_t total_latency;
    uint64_t used_latency;
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

#define ROBFLEX_MAX_LOCAL_EVENT_CTX 8

typedef struct EventContextEntry {
    int event_idx;
    bool in_use;
    LocalContext ctx;
} EventContextEntry;

typedef struct EventContextTable {
    int count;
    EventContextEntry entries[ROBFLEX_MAX_LOCAL_EVENT_CTX];
} EventContextTable;

#ifdef ROBFLEX_USE_KHASH_EVENT_MAP
KHASH_MAP_INIT_INT(EventMap, LocalContext) // event_idx:int -> LocalContext
typedef khash_t(EventMap)* EventContextMap;

static inline int robflex_event_table_init(EventContextMap *table)
{
    if (table == NULL) {
        return -1;
    }
    *table = kh_init(EventMap);
    return (*table != NULL) ? 0 : -1;
}

static inline void robflex_event_table_destroy(EventContextMap *table)
{
    if (table != NULL && *table != NULL) {
        kh_destroy(EventMap, *table);
        *table = NULL;
    }
}

static inline void robflex_event_table_reset(EventContextMap *table)
{
    if (table != NULL && *table != NULL) {
        kh_clear(EventMap, *table);
    }
}

static inline LocalContext *robflex_event_table_get_ctx(EventContextMap *table, int event_idx)
{
    if (table == NULL || *table == NULL || event_idx <= (int)ROBFLEX_EVENT_NONE) {
        return NULL;
    }
    khint_t k = kh_get(EventMap, *table, event_idx);
    if (k == kh_end(*table)) {
        return NULL;
    }
    return &kh_value(*table, k);
}

static inline int robflex_event_table_upsert(EventContextMap *table, int event_idx, const LocalContext *ctx)
{
    if (table == NULL || *table == NULL || ctx == NULL || event_idx <= (int)ROBFLEX_EVENT_NONE) {
        return -1;
    }
    int absent = 0;
    khint_t key = kh_put(EventMap, *table, event_idx, &absent);
    if (absent < 0) {
        return -1;
    }
    kh_value(*table, key) = *ctx;
    return 0;
}

static inline int robflex_event_table_erase(EventContextMap *table, int event_idx)
{
    if (table == NULL || *table == NULL || event_idx <= (int)ROBFLEX_EVENT_NONE) {
        return -1;
    }
    khint_t k = kh_get(EventMap, *table, event_idx);
    if (k == kh_end(*table)) {
        return -1;
    }
    kh_del(EventMap, *table, k);
    return 0;
}

#else
typedef EventContextTable EventContextMap;

static inline int robflex_event_table_init(EventContextMap *table)
{
    if (table == NULL) {
        return -1;
    }
    table->count = 0;
    for (int i = 0; i < ROBFLEX_MAX_LOCAL_EVENT_CTX; ++i) {
        table->entries[i].event_idx = (int)ROBFLEX_EVENT_NONE;
        table->entries[i].in_use = false;
    }
    return 0;
}

static inline void robflex_event_table_destroy(EventContextMap *table)
{
    (void)table;
}

static inline EventContextEntry *robflex_event_table_find_entry(EventContextMap *table, int event_idx)
{
    if (table == NULL || event_idx <= (int)ROBFLEX_EVENT_NONE) {
        return NULL;
    }
    for (int i = 0; i < ROBFLEX_MAX_LOCAL_EVENT_CTX; ++i) {
        EventContextEntry *entry = &table->entries[i];
        if (entry->in_use && entry->event_idx == event_idx) {
            return entry;
        }
    }
    return NULL;
}

static inline int robflex_event_table_upsert(EventContextMap *table, int event_idx, const LocalContext *ctx)
{
    if (table == NULL || ctx == NULL || event_idx <= (int)ROBFLEX_EVENT_NONE) {
        return -1;
    }

    EventContextEntry *existing = robflex_event_table_find_entry(table, event_idx);
    if (existing != NULL) {
        existing->ctx = *ctx;
        return 0;
    }

    for (int i = 0; i < ROBFLEX_MAX_LOCAL_EVENT_CTX; ++i) {
        EventContextEntry *entry = &table->entries[i];
        if (!entry->in_use) {
            entry->in_use = true;
            entry->event_idx = event_idx;
            entry->ctx = *ctx;
            table->count++;
            return 0;
        }
    }

    return -1;
}

static inline int robflex_event_table_erase(EventContextMap *table, int event_idx)
{
    EventContextEntry *entry = robflex_event_table_find_entry(table, event_idx);
    if (entry == NULL) {
        return -1;
    }
    entry->in_use = false;
    entry->event_idx = (int)ROBFLEX_EVENT_NONE;
    if (table->count > 0) {
        table->count--;
    }
    return 0;
}

static inline LocalContext *robflex_event_table_get_ctx(EventContextMap *table, int event_idx)
{
    EventContextEntry *entry = robflex_event_table_find_entry(table, event_idx);
    return (entry == NULL) ? NULL : &entry->ctx;
}
#endif

#endif // ROBFLEX_DEF_H