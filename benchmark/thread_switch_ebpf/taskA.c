#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#define MS_TO_NS(x) (x * 1000000ULL)
#define DEFAULT_PERIOD (MS_TO_NS(100))  // 100ms 周期
#define DEFAULT_QUOTA  (MS_TO_NS(40))   // 每周期运行 50ms
#define FIFO_PRIORITY 50  // 与 task_cycle2 相同优先级，形成公平竞争
#define BIND_CPU 0        // 与 task_cycle2 绑定同一 CPU，形成竞争

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int set_scheduler_fifo(void) {
    struct sched_param param;
    param.sched_priority = FIFO_PRIORITY;
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        perror("sched_setscheduler");
        return -1;
    }
    printf("SCHED_FIFO enabled, priority=%d\n", FIFO_PRIORITY);
    return 0;
}

void fake_load(uint64_t expected_ns) {
    uint64_t start = now_ns();
    volatile int n = 0;
    for (int i = 0; i < expected_ns/3; i++) {
        n = n * 44314 + 314159;
    }
}

int main(int argc, char *argv[]) {
    // 先设置 CPU affinity，再设置实时调度策略
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(BIND_CPU, &cpuset);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == -1) {
        perror("sched_setaffinity");
        return -1;
    }
    printf("Bound to CPU %d\n", BIND_CPU);

    if (argc == 2 && atoi(argv[1]) == 1) {
        if (set_scheduler_fifo() != 0) {
            return -1;
        }
    }

    // 控制运行周期为 100ms，每周期运行 fake_load 50ms，剩余时间 sleep
    while (1) {
        uint64_t start_ns = now_ns();
        fake_load(DEFAULT_QUOTA);  // 预期最多运行 50ms
        uint64_t elapsed_ns = now_ns() - start_ns;
        printf("elapsed_us: %llu\n", elapsed_ns/1000ULL);
        if (elapsed_ns < DEFAULT_PERIOD) {
            uint64_t sleep_ns = DEFAULT_PERIOD - elapsed_ns;
            struct timespec rem = {
                .tv_sec  = sleep_ns / 1000000000ULL,
                .tv_nsec = sleep_ns % 1000000000ULL
            };
            nanosleep(&rem, NULL);
        }
    }
}