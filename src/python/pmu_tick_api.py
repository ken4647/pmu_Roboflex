"""
Python bindings for librobonix.so (Roboflex API), via CFFI.

Requires: cffi, and LD_LIBRARY_PATH (or rpath) so librobonix.so can be loaded.
"""
from __future__ import annotations

import os
from cffi import FFI

ffi = FFI()

ffi.cdef(
    """
    typedef int pid_t;
    typedef unsigned long long uint64_t;
    typedef unsigned int uint32_t;

    typedef int atomic_int;
    typedef unsigned long long atomic_ullong;

    typedef enum {
        SYSTEM_IDLE = 0,
        SYSTEM_MODERATE = 1,
        SYSTEM_HIGH = 2
    } SystemBusyDegree;

    typedef enum {
        YIELDING = 0,
        PREDETERMINED = 1,
        IMMEDIATE = 2,
        LATENCY_ORIENTED = 3
    } RunPolicy;

    struct YieldDemand {
        atomic_ullong time_slice_ns;
        long long time_budgets;
    };

    struct LatencyDemand {
        uint64_t target_lat;
        uint64_t start_time;
        uint64_t hist_ncycle;
        uint64_t used_ncycle;
    };

    union uAuxData {
        struct YieldDemand norm;
        struct LatencyDemand lat;
    };

    typedef struct {
        atomic_int in_critical;
        atomic_int n_signal_pendings;
        RunPolicy policy;
        union uAuxData aux;
        uint64_t cycles_num;
        uint64_t avg_timecost_ns;
    } LocalContext;

    int robflex_set_scheduler(pid_t tid, int policy, int priority);
    int robflex_set_priority(pid_t tid, int priority);
    int robflex_log_message(pid_t tid, const char *message, ...);
    int robflex_update_ctrl_time_cost(pid_t tid, int value_in_us);

    int robflex_create_event(const char *event_name);
    int robflex_delete_event(const char *event_name);
    int robflex_apply_event(const char *event_name, uint64_t timeout, int strength);
    int robflex_cancel_event(const char *event_name, int strength);
    int robflex_attach_context_for_event(const char *event_name, RunPolicy epolicy, union uAuxData data, uint32_t level);
    int robflex_dettach_context_for_event(const char *event_name);
    int robflex_get_policy_for_event(const char *event_name);
    _Bool robflex_test_event(int event_idx);

    int robflex_init_local_context(RunPolicy mode);
    int robflex_set_cycles_for_tick(uint64_t cycles);
    int robflex_clear_time_budget(void);
    int robflex_set_time_for_throttle(uint64_t time_ns);
    int robflex_set_as_immediate(uint64_t prio);
    int robflex_set_as_latency_flick(uint64_t lat_ns, uint64_t base_ns);
    int robflex_add_runcycle(uint64_t runcycle);
    int robflex_shot_on_latency(void);

    int robflex_switch_context(LocalContext *new_context, LocalContext *saved_context);
    int robflex_switch_context_block(LocalContext *new_context, LocalContext *saved_context);

    SystemBusyDegree robflex_system_busy_degree(void);
    """
)

_lib_candidates = [
    os.environ.get("ROBFLEX_LIB"),
    "librobonix.so",
    os.path.join(os.path.dirname(__file__), "..", "..", "build", "librobonix.so"),
]


def _load_lib():
    last = None
    for path in _lib_candidates:
        if not path:
            continue
        try:
            return ffi.dlopen(path)
        except OSError as e:
            last = e
    raise OSError(
        "Could not load librobonix.so. Set ROBFLEX_LIB or extend _lib_candidates. "
        f"Last error: {last}"
    ) from last


lib = _load_lib()

# Linux sched.h policies (for robflex_set_scheduler)
SCHED_NORMAL = 0  # SCHED_OTHER
SCHED_FIFO = 1
SCHED_RR = 2
SCHED_BATCH = 3
SCHED_ISO = 4
SCHED_IDLE = 5
SCHED_DEADLINE = 6

class RunPolicy:
    YIELDING = 0
    PREDETERMINED = 1
    IMMEDIATE = 2
    LATENCY_ORIENTED = 3


class SystemBusyDegree:
    SYSTEM_IDLE = 0
    SYSTEM_MODERATE = 1
    SYSTEM_HIGH = 2


def robflex_set_scheduler(tid: int, policy: int, priority: int) -> int:
    return lib.robflex_set_scheduler(tid, policy, priority)


def robflex_set_priority(tid: int, priority: int) -> int:
    return lib.robflex_set_priority(tid, priority)


def robflex_log_message(tid: int, fmt: str, *args) -> int:
    """Format message in Python, then send a single string to the daemon (vsnprintf-safe)."""
    msg = (fmt % args) if args else fmt
    if isinstance(msg, str):
        msg = msg.encode("utf-8")
    return lib.robflex_log_message(tid, msg)


def robflex_update_ctrl_time_cost(tid: int, value_in_us: int) -> int:
    return lib.robflex_update_ctrl_time_cost(tid, value_in_us)


def _as_c_utf8(name: str | bytes) -> bytes:
    return name if isinstance(name, bytes) else name.encode("utf-8")


def robflex_create_event(event_name: str | bytes) -> int:
    return lib.robflex_create_event(_as_c_utf8(event_name))


def robflex_delete_event(event_name: str | bytes) -> int:
    return lib.robflex_delete_event(_as_c_utf8(event_name))


def robflex_apply_event(event_name: str | bytes, timeout: int, strength: int) -> int:
    return lib.robflex_apply_event(_as_c_utf8(event_name), timeout, strength)


def robflex_cancel_event(event_name: str | bytes, strength: int) -> int:
    return lib.robflex_cancel_event(_as_c_utf8(event_name), strength)


def robflex_attach_context_for_event(
    event_name: str | bytes, epolicy: int, data, level: int
) -> int:
    """data: ffi cdata for union uAuxData (pointer or value), e.g. ffi.new('union uAuxData *')."""
    td = ffi.typeof(data)
    udata = data[0] if td.kind == "pointer" else data
    return lib.robflex_attach_context_for_event(_as_c_utf8(event_name), epolicy, udata, level)


def robflex_dettach_context_for_event(event_name: str | bytes) -> int:
    return lib.robflex_dettach_context_for_event(_as_c_utf8(event_name))


def robflex_get_policy_for_event(event_name: str | bytes) -> int:
    return lib.robflex_get_policy_for_event(_as_c_utf8(event_name))


def robflex_test_event(event_idx: int) -> bool:
    return bool(lib.robflex_test_event(event_idx))


def robflex_init_local_context(mode: int) -> int:
    return lib.robflex_init_local_context(mode)


def robflex_set_cycles_for_tick(cycles: int) -> int:
    return lib.robflex_set_cycles_for_tick(cycles)


def robflex_clear_time_budget() -> int:
    return lib.robflex_clear_time_budget()


def robflex_set_time_for_throttle(time_ns: int) -> int:
    return lib.robflex_set_time_for_throttle(time_ns)


def robflex_set_as_immediate(prio: int) -> int:
    return lib.robflex_set_as_immediate(prio)


def robflex_set_as_latency_flick(lat_ns: int, base_ns: int) -> int:
    return lib.robflex_set_as_latency_flick(lat_ns, base_ns)


def robflex_add_runcycle(runcycle: int) -> int:
    return lib.robflex_add_runcycle(runcycle)


def robflex_shot_on_latency() -> int:
    return lib.robflex_shot_on_latency()


def robflex_switch_context(new_context, saved_context=None) -> int:
    """new_context / saved_context: ffi cdata LocalContext* or None."""
    return lib.robflex_switch_context(new_context, saved_context)


def robflex_switch_context_block(new_context, saved_context=None) -> int:
    return lib.robflex_switch_context_block(new_context, saved_context)


def robflex_system_busy_degree() -> int:
    return int(lib.robflex_system_busy_degree())


def localcontext_alloc():
    """Allocate a zeroed LocalContext on the C heap (for switch_context)."""
    return ffi.new("LocalContext *")


if __name__ == "__main__":
    import time

    def workload(ms):
        for _ in range(ms):
            n = 1
            for _ in range(1000):
                n = n * 43243421 + 89854

    robflex_set_as_latency_flick(20_000_000, 13_000_000)
    while True:
        t0 = time.time()
        workload(10)
        t1 = time.time()
        robflex_shot_on_latency()
        t2 = time.time()
        print(f"Time taken: {t2 - t0} seconds, {t1 - t0} seconds")
