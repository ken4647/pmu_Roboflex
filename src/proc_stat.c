#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <robflex_def.h>
#include <unistd.h>

#define MAX_CPUS (256)  // 当前只统计系统总负载
#define INTERVAL_MS 10000 // 10ms = 10000 微秒

enum SystemBusyDegree get_system_busy_degree(float load_1s) {
    if(load_1s < 30.0){
        return SYSTEM_IDLE;
    }else if(load_1s < 80.0){
        return SYSTEM_MODERATE;
    }else{
        return SYSTEM_HIGH;
    }
}

typedef struct {
    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal;
    unsigned long long total;
    unsigned long long non_idle;
} CPUData;

uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void read_cpu_data(FILE *fp, CPUData *cpus, int *count, int ncpu) {
    char buffer[1024];
    rewind(fp); // 回到文件开头，避免重新打开
    int i = 0;

    while (fgets(buffer, sizeof(buffer), fp) && i < ncpu + 1) {
        if (strncmp(buffer, "cpu", 3) != 0) break; // 只读以 cpu 开头的行

        CPUData *c = &cpus[i];
        char name[16];
        // 快速解析字段
        sscanf(buffer, "%s %llu %llu %llu %llu %llu %llu %llu %llu",
               name, &c->user, &c->nice, &c->system, &c->idle,
               &c->iowait, &c->irq, &c->softirq, &c->steal);

        c->non_idle = c->user + c->nice + c->system + c->irq + c->softirq + c->steal;
        c->total = c->non_idle + c->idle + c->iowait;
        i++;
    }
    *count = i;
}

// index = 0 表示总负载，index > 0 表示对应 CPU 核心的负载
float read_system_load(CPUData *cur, CPUData *prev, int index) {
    if (index < 0 || index >= MAX_CPUS + 1) {
        fprintf(stderr, "无效的 CPU 索引: %d\n", index);
        return -1;
    }
    unsigned long long total_diff = cur[index].total - prev[index].total;
    unsigned long long idle_diff = (cur[index].idle + cur[index].iowait) - (prev[index].idle + prev[index].iowait);

    float usage = 0.0;
    if (total_diff > 0) {
        usage = (float)(total_diff - idle_diff) / total_diff * 100.0;
    }    

    return usage;
}

static SystemData *ptr_shmem = NULL;
void* get_shmem_data(const char *shmem_name){
    if(ptr_shmem == NULL){
        int fd = shm_open(shmem_name, O_RDWR | O_CREAT, 0666);
        if(fd == -1){
            perror("shm_open");
            return NULL;
        }
        if(ftruncate(fd, sizeof(SystemData)) == -1){
            perror("ftruncate");
            close(fd);
            return NULL;
        }
        ptr_shmem = (SystemData *)mmap(NULL, sizeof(SystemData), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if(ptr_shmem == MAP_FAILED){
            perror("mmap");
            ptr_shmem = NULL;
        }
        close(fd);
    }
    return (void*)ptr_shmem;
}

int main() {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) {
        perror("无法打开 /proc/stat");
        return 1;
    }

    SystemData *data = get_shmem_data(SHMEM_NAME);
    if(data == NULL){
        perror("get_shmem_data");
        return 1;
    }

    int online_ncpu = sysconf(_SC_NPROCESSORS_ONLN);

    CPUData prev[online_ncpu + 1];
    CPUData curr[online_ncpu + 1];
    int cpu_count = 0;

    // 初始采样, 读取所有CPU核心的负载
    read_cpu_data(fp, prev, &cpu_count, online_ncpu);
    float load_1s = 30.0;

    while (1) {
        usleep(INTERVAL_MS); // 等待 10ms

        // 统计这段代码的时间开销
        // uint64_t start_time = get_time_ns();
        uint64_t current_time = get_time_ns();
        read_cpu_data(fp, curr, &cpu_count, online_ncpu);

        // only update date for total load
        float load_10ms = read_system_load(curr, prev, 0);
        load_1s = load_1s * 0.9 + load_10ms * 0.1;
        // printf("load_1s: %f\n", load_1s);

        atomic_store(&ptr_shmem->load_10ms, load_10ms);
        atomic_store(&ptr_shmem->load_1s, load_1s);
        atomic_store(&ptr_shmem->last_update_time, current_time);
        atomic_store(&ptr_shmem->busy_degree, get_system_busy_degree(load_1s));

        prev[0] = curr[0];
        // uint64_t end_time = get_time_ns();
        // printf("time cost: %lld ns\n", end_time - start_time);

    }

    fclose(fp);
    return 0;
}