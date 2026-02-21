# Makefile
CC = gcc

CFLAGS = -Wall -Wextra -O2
ifdef DEBUG 
	CFLAGS += -O0 -g
endif
LDFLAGS = -static

.PHONY: all clean test

all: server

server: server.o

clean:
	rm -f *.o server

test: server
	@echo "Starting tests"; \
	./server & SERVER_PID=$$!; \
    timeout 3 bash -c 'until nc -z localhost 6379; do sleep 0.1; done'; \
	echo "SET foo bar" | nc -w 1 -N localhost 6379 | grep -q "SET foo bar"; \
	echo "GET foo" | nc -w 1 -N localhost 6379 | grep -q "GET foo"; \
	echo "DEL foo" | nc -w 1 -N localhost 6379 | grep -q "DEL foo"; \
	kill $$SERVER_PID

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
