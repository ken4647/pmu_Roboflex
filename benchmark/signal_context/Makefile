CC := gcc
CFLAGS := -Wall -Wextra -O2 -I../../inc
# CMake target add_library(robonix SHARED ...) -> build/librobonix.so
LDFLAGS := -L../../build -Wl,-rpath,'$$ORIGIN/../../build' -lrobonix -lpthread

ROBO_SO := ../../build/librobonix.so
# Rebuild shared lib when hook/API sources change (avoids stale librobonix.so missing new symbols).
LIBROBO_DEPS := ../../src/dym_hook.c ../../src/robflex_api.c ../../CMakeLists.txt

all: $(ROBO_SO) test_signal_context
	./test_signal_context

$(ROBO_SO): $(LIBROBO_DEPS)
	cd ../.. && cmake -B build -S . && cmake --build build

test_signal_context: test.c $(ROBO_SO)
	$(CC) $(CFLAGS) -o $@ test.c $(LDFLAGS)

clean:
	rm -f test_signal_context

.PHONY: all clean
