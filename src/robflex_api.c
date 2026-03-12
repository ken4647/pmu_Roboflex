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


