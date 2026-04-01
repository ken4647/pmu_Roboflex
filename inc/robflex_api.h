// robflex_api.h
// forward declaration of the API functions
// Implement in robflex.so
#ifndef ROBFLEX_API_H
#define ROBFLEX_API_H

#define _GNU_SOURCE

#include <robflex_def.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

// env variable reader
// inline unsigned long long get_perf_ctrl_cpi() implemented in dym_hook.c, as it needs to read env variable only
// once and store in atomic variable for later use by signal handler
inline unsigned long long get_instr_slice(){
    const char *env = getenv(ROBFLEX_INSTRUCTION_SLICE_ENV);
    if (env != NULL) {
        return max(1000,strtoull(env, NULL, 10));
    } else {
        return DEFUALT_PERIOD_CYCLES_NUM; // default value (100 million instructions)
    }
}

inline bool is_robflex_enabled() {
    const char *env = getenv(ROBFLEX_ENABLE_ENV);
    return (env != NULL && strcmp(env, "1") == 0);
}

inline enum RunMode get_runmode_env(){
    const char *env = getenv(ROBFLEX_RUNMODE_ENV);
    if (env != NULL) {
        if (strcmp(env, "0") == 0 || strcmp(env, "YIELDING") == 0) {
            return YIELDING;
        }
        if (strcmp(env, "1") == 0 || strcmp(env, "PREDETERMINED") == 0) {
            return PREDETERMINED;
        }
        if (strcmp(env, "2") == 0 || strcmp(env, "IMMEDIATE") == 0) {
            return IMMEDIATE;
        }
        if (strcmp(env, "3") == 0 || strcmp(env, "LATENCY_ORIENTED") == 0) {
            return LATENCY_ORIENTED;
        }
    }
    return YIELDING;
}

// handler for cycles tick signal from PMI
void handle_tick();

// daemon related
int robflex_set_scheduler(pid_t tid, int policy, int priority);
int robflex_set_priority(pid_t tid, int priority);
int robflex_log_message(pid_t tid, const char *message, ...);
int robflex_update_ctrl_time_cost(pid_t tid, int value_in_us);

// local context management
extern __thread LocalContext loc_ctx;
int robflex_init_local_context(enum RunMode mode);
int robflex_set_cycles_for_tick(uint64_t cycles);
int robflex_set_time_for_throttle(uint64_t time_ns);
int robflex_set_as_immediate(uint64_t prio);    // will sched it as fifo
int robflex_set_as_latency_flick(uint64_t lat_ns, uint64_t base_ns);  // set latency finish
int robflex_add_runcycle(uint64_t runcycle);
int robflex_shot_on_latency();                  // clear latency inner status, and restart time point
int robflex_switch_context(LocalContext *new_context, LocalContext *saved_context);
int robflex_switch_context_block(LocalContext *new_context, LocalContext *saved_context); // used for test

// system context management
enum SystemBusyDegree robflex_system_busy_degree();

#endif // ROBFLEX_API_H