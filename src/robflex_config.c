#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include <sys/syscall.h>

#include <cJSON.h>
#include <robflex_def.h>
#include <robflex_api.h>
#include <robflex_config.h>

static pid_t config_gettid(void)
{
    return (pid_t)syscall(__NR_gettid);
}

static uint64_t config_monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static enum RunPolicy parse_policy_string(const char *s)
{
    if (s == NULL) {
        return YIELDING;
    }
    if (strcmp(s, "yielding") == 0) {
        return YIELDING;
    }
    if (strcmp(s, "predetermined") == 0) {
        return PREDETERMINED;
    }
    if (strcmp(s, "immediate") == 0) {
        return IMMEDIATE;
    }
    if (strcmp(s, "latency_oriented") == 0) {
        return LATENCY_ORIENTED;
    }
    if (strcmp(s, "by_handler") == 0) {
        return BY_HANDLER;
    }
    return YIELDING;
}

static int apply_policy_object_to_local_context(cJSON *policy_obj, LocalContext *ctx)
{
    cJSON *jpolicy = cJSON_GetObjectItem(policy_obj, "policy");
    if (!cJSON_IsString(jpolicy) || jpolicy->valuestring == NULL) {
        return -1;
    }

    ctx->policy = parse_policy_string(jpolicy->valuestring);
    ctx->avg_timecost_ns = DEFUALT_PERIOD_TIME_IN_NS;

    cJSON *jlevel = cJSON_GetObjectItem(policy_obj, "level");
    if (cJSON_IsNumber(jlevel)) {
        ctx->level = (int)jlevel->valuedouble;
    } else {
        ctx->level = 0;
    }

    cJSON *jperiod_ns = cJSON_GetObjectItem(policy_obj, "period_time_ns");
    uint64_t time_slice_ns = DEFUALT_PERIOD_TIME_IN_NS;
    if (cJSON_IsNumber(jperiod_ns) && jperiod_ns->valuedouble >= 1.0) {
        time_slice_ns = (uint64_t)jperiod_ns->valuedouble;
    }
    atomic_store_explicit(&ctx->aux.norm.time_slice_ns, time_slice_ns, memory_order_relaxed);
    ctx->aux.norm.time_budgets = 0;

    cJSON *jcycles = cJSON_GetObjectItem(policy_obj, "period_cycles_quota");
    if (cJSON_IsNumber(jcycles) && jcycles->valuedouble >= 1.0) {
        ctx->cycles_num = (uint64_t)jcycles->valuedouble;
    } else {
        ctx->cycles_num = DEFUALT_PERIOD_CYCLES_NUM;
    }

    if (ctx->policy == LATENCY_ORIENTED) {
        ctx->aux.lat.target_lat = time_slice_ns;
        ctx->aux.lat.start_time = config_monotonic_ns();
        ctx->aux.lat.hist_ncycle = ctx->cycles_num;
        ctx->aux.lat.used_ncycle = 0;
    }

    return 0;
}

int parse_config_file(cJSON *config_json)
{
    cJSON *policy_table = cJSON_GetObjectItem(config_json, "policy_table");
    if (policy_table == NULL || !cJSON_IsObject(policy_table)) {
        return 1;
    }

    cJSON *default_policy = cJSON_GetObjectItem(policy_table, "default");
    if (default_policy == NULL || !cJSON_IsObject(default_policy)) {
        return 1;
    }

    if (apply_policy_object_to_local_context(default_policy, &loc_ctx) != 0) {
        return 1;
    }

    cJSON *entry;
    cJSON_ArrayForEach(entry, policy_table)
    {
        const char *key = entry->string;
        if (key == NULL || strcmp(key, "default") == 0) {
            continue;
        }
        if (!cJSON_IsObject(entry)) {
            continue;
        }

        enum RobflexEventIdx ev = robflex_get_event_idx(key);
        if (ev == ROBFLEX_EVENT_NONE) {
            continue;
        }

        int event_idx = (int)ev;
        LocalContext ev_ctx;
        memset(&ev_ctx, 0, sizeof(ev_ctx));
        if (apply_policy_object_to_local_context(entry, &ev_ctx) != 0) {
            return 1;
        }

        if (robflex_event_table_upsert(&event_map, event_idx, &ev_ctx) != 0) {
            return 1;
        }
    }

    return 0;
}

int setup_config_from_file(const char *config_path_in)
{
    const char *path = config_path_in ? config_path_in : getenv(ROBFLEX_CONFIG_PATH_ENV);
    if (path == NULL) {
        return 1;
    }
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        perror("fopen");
        return 1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 1;
    }
    long sz = ftell(file);
    if (sz < 0 || sz > 1024 * 1024) {
        fclose(file);
        return 1;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 1;
    }

    char *buf = malloc((size_t)sz + 1u);
    if (buf == NULL) {
        fclose(file);
        return 1;
    }
    size_t total = 0;
    while (total < (size_t)sz) {
        size_t n = fread(buf + total, 1, (size_t)sz - total, file);
        if (n == 0) {
            break;
        }
        total += n;
    }
    if (total != (size_t)sz || ferror(file)) {
        free(buf);
        fclose(file);
        return 1;
    }
    fclose(file);
    buf[total] = '\0';

    cJSON *config_json = cJSON_Parse(buf);
    free(buf);
    if (config_json == NULL) {
        return 1;
    }

    if (parse_config_file(config_json) != 0) {
        robflex_log_message(config_gettid(), "parse_config_file failed");
        cJSON_Delete(config_json);
        return 1;
    }

    cJSON_Delete(config_json);
    return 0;
}
