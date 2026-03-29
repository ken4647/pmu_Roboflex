#define _POSIX_C_SOURCE 200809L
#include <robflex_api.h>
#include <cJSON.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <time.h>

__thread int socket_fd = 0;
int send_command_to_daemon(const char *cmd_json) {
    if (socket_fd == 0) {
        socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
        if (socket_fd < 0) {
            perror("socket");
            return -1;
        }
    }

    struct sockaddr_un server_addr;
    memset(&server_addr, 0, sizeof(struct sockaddr_un));
    server_addr.sun_family = AF_UNIX;
    strncpy(server_addr.sun_path, SOCKET_PATH, sizeof(server_addr.sun_path) - 1);

    ssize_t num_bytes = sendto(socket_fd, cmd_json, strlen(cmd_json), 0,
                               (struct sockaddr *)&server_addr, sizeof(struct sockaddr_un));
    if (num_bytes < 0) {
        // perror("sendto");
        return -1;
    }
    return 0;
}

int robflex_set_scheduler(pid_t tid, int policy, int priority) {
    char cmd_json[MAX_JSON_SIZE];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "sched_setscheduler");
    cJSON_AddNumberToObject(root, "tid", tid? tid : gettid());
    cJSON_AddNumberToObject(root, "policy", policy);
    cJSON_AddNumberToObject(root, "priority", priority);

    char *cmd_str = cJSON_PrintUnformatted(root);
    int result = send_command_to_daemon(cmd_str);

    cJSON_Delete(root);
    free(cmd_str);

    return result;
}

int robflex_set_priority(pid_t tid, int priority) {
    char cmd_json[MAX_JSON_SIZE];

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "set_priority");
    cJSON_AddNumberToObject(root, "tid", tid? tid : gettid());
    cJSON_AddNumberToObject(root, "priority", priority);

    char *cmd_str = cJSON_PrintUnformatted(root);
    int result = send_command_to_daemon(cmd_str);

    cJSON_Delete(root);
    free(cmd_str);

    return result;
}

int robflex_log_message(pid_t tid, const char *message, ...) {
    char cmd_json[MAX_JSON_SIZE];
    char formatted_message[MAX_LOG_SIZE];
    int result = -1;

    if(!tid){
        tid = gettid();
    }

    va_list args;
    va_start(args, message);
    vsnprintf(formatted_message, sizeof(formatted_message), message, args);
    va_end(args);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "log_message");
    cJSON_AddNumberToObject(root, "tid", tid);
    cJSON_AddStringToObject(root, "message", formatted_message);

    char *cmd_str = cJSON_PrintUnformatted(root);
    result = send_command_to_daemon(cmd_str);

    cJSON_Delete(root);
    free(cmd_str);

    return result;
}

int robflex_update_ctrl_time_cost(pid_t tid, int value_in_us) {
    char cmd_json[MAX_JSON_SIZE];
    int result = -1;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "cmd", "update_ctrl_time_cost");
    cJSON_AddNumberToObject(root, "tid", tid? tid : gettid());
    cJSON_AddNumberToObject(root, "value_in_us", value_in_us);

    char *cmd_str = cJSON_PrintUnformatted(root);
    result = send_command_to_daemon(cmd_str);

    cJSON_Delete(root);
    free(cmd_str);

    return result;
}

static SystemData *ptr_shmem = NULL;
void* _init_shmem_data(const char *shmem_name){
    if(ptr_shmem == NULL){
        int fd = shm_open(shmem_name, O_RDONLY, 0);
        if(fd == -1){   
            perror("shm_open");
            return NULL;
        }
        ptr_shmem = (SystemData *)mmap(NULL, sizeof(SystemData), PROT_READ, MAP_SHARED, fd, 0);
        if(ptr_shmem == MAP_FAILED){
            perror("mmap");
            ptr_shmem = NULL;
        }
        close(fd);
    }
    return ptr_shmem;
}

enum SystemBusyDegree robflex_system_busy_degree() {
    assert(ptr_shmem != NULL);
    return atomic_load(&ptr_shmem->busy_degree);
}

static uint64_t robflex_get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

// assume signal handler is not running
int robflex_init_local_context(){
    loc_ctx.avg_timecost_ns = DEFUALT_PERIOD_TIME_IN_NS;
    loc_ctx.run_mode = PREDETERMINED;
    atomic_store_explicit(&loc_ctx.aux.norm.time_slice_ns, DEFUALT_PERIOD_TIME_IN_NS, memory_order_relaxed);
    loc_ctx.aux.norm.time_budgets = 0;
    atomic_store_explicit(&loc_ctx.in_critical, 0, memory_order_relaxed);
    atomic_store_explicit(&loc_ctx.n_signal_pendings, 0, memory_order_relaxed);
    return 0;
}

int robflex_set_cycles_for_tick(uint64_t cycles){
    atomic_store(&loc_ctx.in_critical, 1);
    if(loc_ctx.run_mode != YIELDING && loc_ctx.run_mode != PREDETERMINED){
        return -1;
    }
    // TODO: reset cycles_num for perf_fd
    
    loc_ctx.aux.norm.time_budgets = 0;
    atomic_store(&loc_ctx.in_critical, 0);
    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }
    return 0;
}

int robflex_set_time_for_throttle(uint64_t time_ns){
    atomic_store(&loc_ctx.in_critical, 1);
    if(loc_ctx.run_mode != YIELDING && loc_ctx.run_mode != PREDETERMINED){
        return -1;
    }
    loc_ctx.aux.norm.time_slice_ns = time_ns;
    loc_ctx.aux.norm.time_budgets = 0;
    atomic_store(&loc_ctx.in_critical, 0);
    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }
    return 0;
}

int robflex_set_as_immediate(uint64_t prio){
    loc_ctx.run_mode = IMMEDIATE;
    return 0;
}

int robflex_set_as_latency_flick(uint64_t lat_ns, uint64_t base_ns){
    atomic_store(&loc_ctx.in_critical, 1);
    loc_ctx.run_mode = LATENCY_ORIENTED;
    loc_ctx.aux.lat.target_lat = lat_ns;
    loc_ctx.aux.lat.start_time = robflex_get_time_ns();
    loc_ctx.aux.lat.hist_ncycle = base_ns;
    atomic_store(&loc_ctx.in_critical, 0);
    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }
    return 0;
}

int robflex_add_runcycle(uint64_t runcycle){
    atomic_store(&loc_ctx.in_critical, 1);
    loc_ctx.aux.lat.used_ncycle += runcycle;
    atomic_store(&loc_ctx.in_critical, 0);
    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }
    return 0;
}

int robflex_shot_on_latency(){
    atomic_store(&loc_ctx.in_critical, 1);
    if(loc_ctx.run_mode != LATENCY_ORIENTED){
        return -1;
    }
    uint64_t current_time = robflex_get_time_ns();
    uint64_t pass_time = current_time - loc_ctx.aux.lat.start_time;
    long long rest_time = (long long)loc_ctx.aux.lat.target_lat - (long long)pass_time;
    loc_ctx.aux.lat.hist_ncycle = loc_ctx.aux.lat.hist_ncycle*0.5 + loc_ctx.aux.lat.used_ncycle*0.5;
    // loc_ctx.aux.lat.start_time = current_time;
    loc_ctx.aux.lat.used_ncycle = 0;
    atomic_store(&loc_ctx.in_critical, 0);

    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }

    // printf("target latency: %llu, pass time: %llu, rest time: %lld, hist_ncycle: %llu, used_ncycle: %llu\n", loc_ctx.aux.lat.target_lat, pass_time, rest_time, loc_ctx.aux.lat.hist_ncycle, loc_ctx.aux.lat.used_ncycle);
    if(rest_time > 1000000) {
        struct timespec sleep_ts;
        sleep_ts.tv_sec = 0;
        sleep_ts.tv_nsec = rest_time - 1000000;
        robflex_log_message(gettid(), "rest_time:%llu, sleep_ns:%llu", rest_time, rest_time - 1000000);
        nanosleep(&sleep_ts, NULL);
    }

    loc_ctx.aux.lat.start_time = robflex_get_time_ns();
    return 0;
}

int robflex_switch_context(LocalContext *new_context, LocalContext *saved_context){
    LocalContext loc_ctx_backup;
    atomic_store(&loc_ctx.in_critical, 1);
    if(saved_context != NULL){
        loc_ctx_backup = loc_ctx;
    }
    loc_ctx = *new_context;
    atomic_store(&loc_ctx.in_critical, 0);
    while(atomic_exchange(&loc_ctx.n_signal_pendings, 0)){
        handle_tick();
    }
    // probably saved_context is new_context, so we need to copy the data from loc_ctx_backup to saved_context
    if(saved_context != NULL){
        *saved_context = loc_ctx_backup;
    }
    return 0;
}

// only for test
int robflex_switch_context_block(LocalContext *new_context, LocalContext *saved_context){
    LocalContext loc_ctx_backup;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, PERF_SIGNAL);

    sigprocmask(SIG_BLOCK, &set, NULL);
    if(saved_context != NULL){
        loc_ctx_backup = loc_ctx;
    }
    loc_ctx = *new_context;
    sigprocmask(SIG_UNBLOCK, &set, NULL);

    // probably saved_context is new_context, so we need to copy the data from loc_ctx_backup to saved_context
    if(saved_context != NULL){
        *saved_context = loc_ctx_backup;
    }    
    return 0;
}