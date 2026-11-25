CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
BIN := bin
SOURCES := main.c server.c client.c protocol.c reader.c writer.c observer.c
OBJECTS := $(SOURCES:.c=.o)

all: $(BIN)/idz4

$(BIN):
	mkdir -p $(BIN)

$(BIN)/idz4: $(OBJECTS) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c $< -o $@

client.o reader.o writer.o observer.o: client_internal.h

server.o: common.h

protocol.o: common.h

test: all
	bash ./tests.sh

clean:
	rm -f $(OBJECTS)
	rm -rf $(BIN) logs

.PHONY: all test clean
