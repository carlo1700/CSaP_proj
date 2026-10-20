CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -Werror -O2
LDLIBS = -lrt

SERVER_OBJECTS = server.o session.o transfer.o filesystem.o privilege.o \
                 network.o utils.o
CLIENT_OBJECTS = client.o client_transfer.o network.o utils.o

.PHONY: all clean

all: Server Client

Server: $(SERVER_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

Client: $(CLIENT_OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c $(wildcard *.h)
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f Server Client *.o
