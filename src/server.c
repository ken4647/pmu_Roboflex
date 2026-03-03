// 基于Unix Socket实现调度策略设置并提供基础的日志服务
// 主要功能：
// 1. 创建Unix Socket服务器，监听来自客户端的连接请求
// 2. 接收客户端发送的调度策略设置命令，并执行相应的系统调用来设置调度策略
// 3. 使用json格式进行通信，方便扩展和解析
// 4. 策略修改通常为低频操作，只使用单线程+SOCK_DGRAM即可满足需求，不需要复杂的多线程处理
// 5. 需要实现的接口包括， Json字段完全根据Glibc函数字段设计即可：
//    - sched_setscheduler(tid, policy, priority) ，要求tid!=0，否则不知道是哪个线程需要设置
//    - setpriority(PRIO_PROCESS, tid, priority) ，特别要求tid!=0，否则不知道是哪个线程需要设置
//    - log_message(tid, message) 记录日志消息，要求tid!=0，表示哪个线程发送的日志
// 6. 需要检查CAP_SYS_NICE权限，确保有权限设置调度策略

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/capability.h>
#include <sys/resource.h>
#include <sched.h>

#include <cJSON.h>

#include <robflex_def.h>

struct UnixSocketServer {
    int socket_fd;
    struct sockaddr_un addr;
};

int setup_unix_socket(struct UnixSocketServer *server) {
    // 创建Unix Socket
    server->socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        perror("socket");
        return -1;
    }

    // 设置地址结构
    memset(&server->addr, 0, sizeof(struct sockaddr_un));
    server->addr.sun_family = AF_UNIX;
    strncpy(server->addr.sun_path, SOCKET_PATH, sizeof(server->addr.sun_path) - 1);

    // 绑定Socket
    if (bind(server->socket_fd, (struct sockaddr *)&server->addr, sizeof(struct sockaddr_un)) < 0) {
        perror("bind");
        close(server->socket_fd);
        return -1;
    }

    printf("Unix Socket Server is listening on /tmp/schedule_daemon.sock\n");
    return 0;
}

int do_sched_setscheduler(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *policy = cJSON_GetObjectItem(root, "policy");
    cJSON *priority = cJSON_GetObjectItem(root, "priority");

    if (!tid || !policy || !priority) {
        fprintf(stderr, "Missing required fields for sched_setscheduler\n");
        return -1;
    }

    printf("sched_setscheduler: tid: %d, policy: %d, priority: %d\n", tid->valueint, policy->valueint, priority->valueint);

    // SCHED_OTHER时priority应解释为nice值
    if (policy->valueint == SCHED_OTHER) {
        // 依然需要调用sched_setscheduler，但priority字段无效
        struct sched_param param;
        param.sched_priority = 0; // SCHED_OTHER下忽略sched_priority
        if(sched_setscheduler(tid->valueint, SCHED_OTHER, &param) < 0) {
            return -1;
        }
        // set nice to "priority"
        if(setpriority(PRIO_PROCESS, tid->valueint, priority->valueint) < 0) {
            return -1;
        }

        return 0;
    } else {
        struct sched_param param;
        param.sched_priority = priority->valueint;
        return sched_setscheduler(tid->valueint, policy->valueint, &param);
    }
}

int do_set_priority(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *priority = cJSON_GetObjectItem(root, "priority");
    if (!tid || !priority) {
        fprintf(stderr, "Missing required fields for set_priority\n");
        return -1;
    }

    printf("set_priority: tid: %d, priority: %d\n", tid->valueint, priority->valueint);

    return setpriority(PRIO_PROCESS, tid->valueint, priority->valueint);
}

int do_log_message(const cJSON *root) {
    cJSON *tid = cJSON_GetObjectItem(root, "tid");
    cJSON *message = cJSON_GetObjectItem(root, "message");
    if (!tid || !message) {
        fprintf(stderr, "Missing required fields for log_message\n");
        return -1;
    }
    // 记录日志消息，可以根据需要将日志写入文件或输出到控制台
    printf("Log from thread %d: %s\n", tid->valueint, message->valuestring);
    return 0;
}

int cmd_dispatcher(const cJSON *root) {
    cJSON *cmd = cJSON_GetObjectItem(root, "cmd");
    if (!cmd) {
        fprintf(stderr, "Missing 'cmd' field\n");
        return -1;
    }

    if (strcmp(cmd->valuestring, "sched_setscheduler") == 0) {
        return do_sched_setscheduler(root);
    } else if (strcmp(cmd->valuestring, "set_priority") == 0) {
        return do_set_priority(root);
    } else if (strcmp(cmd->valuestring, "log_message") == 0) {
        return do_log_message(root);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd->valuestring);
        return -1;
    }
}

void handle_client_request(struct UnixSocketServer *server) {
    char buffer[256];
    struct sockaddr_un client_addr;
    socklen_t client_addr_len = sizeof(struct sockaddr_un);

    // 接收客户端请求
    ssize_t num_bytes = recvfrom(server->socket_fd, buffer, sizeof(buffer) - 1, 0,
                                 (struct sockaddr *)&client_addr, &client_addr_len);
    if (num_bytes < 0) {
        perror("recvfrom");
        return;
    }
    buffer[num_bytes] = '\0'; // 确保字符串以null结尾

    // 处理不同的命令类型
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        fprintf(stderr, "Error parsing JSON\n");
        return;
    }

    if(cmd_dispatcher(root) != 0) {
        perror("cmd_dispatcher");
        cJSON_Delete(root);
        return;
    }

    cJSON_Delete(root);
    return;
}


int check_capability(cap_value_t cap) {
    cap_t caps = cap_get_proc();
    if (caps == NULL) {
        perror("cap_get_proc");
        return -1;
    }

    cap_flag_value_t has_cap;
    if (cap_get_flag(caps, cap, CAP_EFFECTIVE, &has_cap) == -1) {
        perror("cap_get_flag");
        cap_free(caps);
        return -1;
    }

    cap_free(caps);
    return has_cap == CAP_SET ? 0 : -1;
}

int main() {
    struct UnixSocketServer server;
    
    printf("Schedule Daemon is Starting...\n");

    // 检查CAP_SYS_NICE权限
    if(check_capability(CAP_SYS_NICE) != 0) {
        fprintf(stderr, "Insufficient privileges to set scheduling policy\n");
        return 1;
    }

    // 清除sock addr文件
    unlink(SOCKET_PATH);

    // 设置Unix Socket服务器
    if (setup_unix_socket(&server) != 0) {
        fprintf(stderr, "Failed to set up Unix Socket Server\n");
        return 1;
    }

    while(1){
        handle_client_request(&server);
    }

    return 0;
}

