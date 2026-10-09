CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic
TARGET = idz1
SRC = main.c simulation.c

all: $(TARGET)

$(TARGET): $(SRC) simulation.h
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)