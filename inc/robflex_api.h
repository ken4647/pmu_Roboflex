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

inline bool is_robflex_enabled() {
    const char *env = getenv(ROBFLEX_ENABLE_ENV);
    return (env != NULL && strcmp(env, "1") == 0);
}

// daemon related
int robflex_set_scheduler(pid_t tid, int policy, int priority);
int robflex_set_priority(pid_t tid, int priority);
int robflex_log_message(pid_t tid, const char *message, ...);


#endif // ROBFLEX_API_H