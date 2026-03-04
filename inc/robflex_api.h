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
        return max(1,strtoull(env, NULL, 10))*1000*1000;
    } else {
        return DEFUALT_PERF_SIG_PER_INSTRUCTION; // default value (100 million instructions)
    }
}

inline bool is_robflex_enabled() {
    const char *env = getenv(ROBFLEX_ENABLE_ENV);
    return (env != NULL && strcmp(env, "1") == 0);
}

// daemon related
int robflex_set_scheduler(pid_t tid, int policy, int priority);
int robflex_set_priority(pid_t tid, int priority);
int robflex_log_message(pid_t tid, const char *message, ...);
int robflex_update_ctrl_time_cost(pid_t tid, int value_in_ms);


#endif // ROBFLEX_API_H