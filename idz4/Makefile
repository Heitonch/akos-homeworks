CC ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE
BIN := bin
all: $(BIN)/idz4
$(BIN):
	mkdir -p $(BIN)
$(BIN)/idz4: main.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ main.c
test: all
	bash ./tests.sh
clean:
	rm -rf $(BIN) logs
.PHONY: all test clean
