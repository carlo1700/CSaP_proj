CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -lrt

COMMON_OBJECTS = protocol.o filesystem.o

.PHONY: all clean

all: Server Client

Server: server.o $(COMMON_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

Client: client.o protocol.o
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f Server Client *.o
