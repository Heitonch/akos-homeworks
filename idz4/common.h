#ifndef IDZ4_COMMON_H
#define IDZ4_COMMON_H

#include <stddef.h>

#define IDZ4_MAX_CLIENTS 128
#define IDZ4_MAX_SIZE 100000
#define IDZ4_LINE_SIZE 512
#define IDZ4_MAX_WAITERS 128

typedef enum {
    STRATEGY_READ_OLD = 0,
    STRATEGY_WAIT_NEW = 1
} ReadStrategy;

typedef struct {
    const char *host;
    const char *port;
    int id;
    int operations;
    unsigned int seed;
} ClientOptions;

int parse_integer(const char *text, long min_value, long max_value, long *value);
int send_line(int fd, const char *line);
int receive_line(int fd, char *buffer, size_t capacity);
int connect_to_server(const char *host, const char *port);
int run_reader(const ClientOptions *options);
int run_writer(const ClientOptions *options);
int run_observer(const ClientOptions *options);
int run_server(int argc, char **argv);

#endif
