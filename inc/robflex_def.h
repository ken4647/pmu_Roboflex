#ifndef ROBFLEX_DEF_H
#define ROBFLEX_DEF_H

#define _POSIX_C_SOURCE 200809L
#include <signal.h>

#define PERF_SIGNAL (SIGRTMIN+1)
#define CPI_SET_SIGNAL (SIGRTMIN+2)

#define ROBFLEX_ENABLE_ENV "ROBFLEX_ENABLE_FEATURES"
#define ROBFLEX_PERF_CTRL_CPI_ENV "ROBFLEX_PERF_CTRL_CPI_1M"
#define ROBFLEX_INSTRUCTION_SLICE_ENV "ROBFLEX_INSTRUCTION_SLICE_1M"

#define DEFUALT_PERF_SIG_PER_INSTRUCTION (20ULL*1000*1000)
#define DEFUALT_TIME_COST_PER_INTERRUPT_NS (10ULL*1000*1000)

#define SOCKET_PATH "/tmp/schedule_daemon.sock"
#define MAX_JSON_SIZE (1024)
#define MAX_LOG_SIZE (512)

#ifndef max
#define max(a,b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a,b) (((a) < (b)) ? (a) : (b))
#endif

#endif // ROBFLEX_DEF_H