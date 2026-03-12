// task_cycle2.c
// 实现每 70ms 内运行 10ms 的假负载，运行完后通过 Unix DGRAM Socket 往 /tmp/bcc_msg.sock 发送一条数据
#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdint.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

#define BCC_MSG_SOCK "/tmp/bcc_msg.sock"

int send_bcc_msg(const char *msg) {
    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, BCC_MSG_SOCK, sizeof(server_addr.sun_path) - 1);

    if (sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un)) < 0) {
        perror("sendto");
        return -1;
    }

    close(sock);
    return 0;
}


#define MS_TO_NS(x) (x * 1000000ULL)
#define DEFAULT_PERIOD  (MS_TO_NS(40))  // 70ms
#define FIFO_PRIORITY 50  // 与 task_cycle1 相同优先级，形成公平竞争
#define BIND_CPU 0        // 与 task_cycle1 绑定同一 CPU，形成竞争

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

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
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

    // 控制运行周期为 DEFAULT_PERIOD (ns)，每周期运行一次 fake_load + send
    uint64_t start_ns = now_ns();
    while (1) {
        // send_bcc_msg("task cycle2 tick");
        fake_load(MS_TO_NS(10));  // 预期最多运行 10ms，超过则提前返回
        send_bcc_msg("task cycle2 tick");
        uint64_t elapsed_ns = now_ns() - start_ns;
        start_ns = start_ns+MS_TO_NS(40);
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

