# Makefile for c-db-k8s
# make all
# make tests
CC = gcc

#CFLAGS = -Wall -Wextra -O2
CFLAGS += -D_GNU_SOURCE -Wall -O2 -Isrc -MMD -MP
ifdef DEBUG 
	CFLAGS += -O0 -g
endif
LDFLAGS = -static

BUILD_DIR = build
SRC_DIR = src
TARGETS = server client

.PHONY: all clean test

all: $(BUILD_DIR) $(TARGETS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# server
SERVER_SRCS = src/util.c src/sock.c src/db.c src/server.c
SERVER_OBJS = $(SERVER_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
SERVER_DEPS = $(SERVER_OBJS:.o=.d)
-include $(SERVER_DEPS)
server: $(SERVER_OBJS)
	$(CC) $(SERVER_OBJS) -o $@

# client
CLIENT_SRCS = src/client.c
CLIENT_OBJS = $(CLIENT_SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
CLIENT_DEPS = $(CLIENT_OBJS:.o=.d)
-include $(CLIENT_DEPS)
client: $(CLIENT_OBJS)
	$(CC) $(CLIENT_OBJS) -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGETS)

test: server
	@echo "Starting tests"; \
	./server & SERVER_PID=$$!; \
    timeout 3 bash -c 'until nc -z localhost 6379; do sleep 0.1; done'; \
	echo "SET foo bar" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "SET failed"; \
	echo "GET foo" | nc -w 1 -N localhost 6379 | grep -q "bar" || echo "GET failed"; \
	echo "DEL foo" | nc -w 1 -N localhost 6379 | grep -q "OK" || echo "DEL failed"; \
	kill $$SERVER_PID

# XXX force BUILD_DIR as a prerequisite
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@
