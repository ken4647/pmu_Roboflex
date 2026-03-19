from cffi import FFI

ffi = FFI()

# 声明函数
ffi.cdef("""
    int robflex_set_as_latency_flick(uint64_t lat_ns, uint64_t base_ns);
    int robflex_shot_on_latency();
""")

# 加载库
lib = ffi.dlopen("librobonix.so")


def robflex_set_as_latency_flick(lat_ns, base_ns):
    lib.robflex_set_as_latency_flick(lat_ns, base_ns)

def robflex_shot_on_latency():
    lib.robflex_shot_on_latency()

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
