CC       := gcc
CFLAGS   := -Wall -Wextra -O2 -fPIC
LDFLAGS  :=

SRC_DIR  := src
INC_DIR  := inc

DAEMON   := robonix_daemon
SO_LIB   := robonix.so

DAEMON_SRC := $(SRC_DIR)/server.c
HOOK_SRC   := $(SRC_DIR)/dym_hook.c $(SRC_DIR)/robflex_api.c

INCLUDES := -I$(INC_DIR)

all: $(DAEMON) $(SO_LIB)

$(DAEMON): $(DAEMON_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $^ -lcjson -lcap

$(SO_LIB): $(HOOK_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -shared -o $@ $^ -ldl -lpthread

clean:
	rm -f $(DAEMON) $(SO_LIB)

.PHONY: all clean