#define _POSIX_C_SOURCE 200809L
#include "client_internal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop_requested;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

void client_install_signal_handlers(void)
{
    stop_requested = 0;
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
}

int client_should_stop(void)
{
    return stop_requested != 0;
}

void client_sleep_ms(int milliseconds)
{
    struct timespec delay;

    delay.tv_sec = milliseconds / 1000;
    delay.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (!client_should_stop() && nanosleep(&delay, &delay) < 0) {
        /* Resume the remaining delay after an unrelated signal. */
    }
}

int client_request(int socket_fd, const char *command, char *response,
                   size_t response_capacity)
{
    return send_line(socket_fd, command) &&
           receive_line(socket_fd, response, response_capacity);
}

int client_start(const char *role, const ClientOptions *options,
                 int *socket_fd, int *database_size, int *stages,
                 int *default_operations, int *read_delay_ms,
                 int *preparation_delay_ms, int *confirmation_delay_ms)
{
    char command[IDZ4_LINE_SIZE];
    char response[IDZ4_LINE_SIZE];
    int version;
    int strategy;
    int connected_fd;

    connected_fd = connect_to_server(options->host, options->port);
    if (connected_fd < 0) {
        return 0;
    }

    (void)snprintf(command, sizeof(command), "HELLO %s %d", role, options->id);
    if (!client_request(connected_fd, command, response, sizeof(response)) ||
        sscanf(response, "WELCOME %d %d %d %d %d %d %d %d",
               &version, database_size, &strategy, stages,
               default_operations, read_delay_ms, preparation_delay_ms,
               confirmation_delay_ms) != 8) {
        fprintf(stderr, "Не удалось выполнить регистрацию клиента\n");
        close(connected_fd);
        return 0;
    }

    (void)version;
    (void)strategy;
    *socket_fd = connected_fd;
    printf("[%s %d] подключение установлено: %s:%s\n",
           role, options->id, options->host, options->port);
    return 1;
}
