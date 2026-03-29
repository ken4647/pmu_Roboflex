#define _GNU_SOURCE
#include <robflex_api.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define ITER 10000000

typedef int (*robflex_switch_fn)(LocalContext *, LocalContext *);

static void fill_predetermined_ctx(LocalContext *c) {
    memset(c, 0, sizeof(*c));
    c->run_mode = PREDETERMINED;
    c->avg_timecost_ns = DEFUALT_PERIOD_TIME_IN_NS;
    atomic_store_explicit(&c->aux.norm.time_slice_ns, DEFUALT_PERIOD_TIME_IN_NS,
                          memory_order_relaxed);
    c->aux.norm.time_budgets = 0;
    atomic_store_explicit(&c->in_critical, 0, memory_order_relaxed);
    atomic_store_explicit(&c->n_signal_pendings, 0, memory_order_relaxed);
}

static void bench_switch(robflex_switch_fn fn, const char *label) {
    LocalContext ctx_a, ctx_b, saved;
    fill_predetermined_ctx(&ctx_a);
    fill_predetermined_ctx(&ctx_b);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long start = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    for (int i = 0; i < ITER; i++) {
        fn(&ctx_b, &saved);
        fn(&ctx_a, &saved);
    }

    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long end = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;

    printf("[%s] avg cost: %.2f ns (over %d switch pairs)\n", label,
           (double)(end - start) / (2 * ITER), ITER);
}

int main(void) {

    printf("Running %d iterations per API (each iteration = 2 context switches)...\n",
           ITER);

    bench_switch(robflex_switch_context, "robflex_switch_context");
    bench_switch(robflex_switch_context_block, "robflex_switch_context_block");

    return 0;
}
