# Makefile
CC = gcc
CFLAGS = -Wall -O2
LDFLAGS = -static

.PHONY: all clean
all: server

server: server.o
clean:
	rm -f *.o

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@
