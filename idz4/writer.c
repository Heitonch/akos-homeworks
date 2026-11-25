#include "client_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

int run_writer(const ClientOptions *options)
{
    char command[IDZ4_LINE_SIZE];
    char response[IDZ4_LINE_SIZE];
    int socket_fd;
    int database_size;
    int stages;
    int default_operations;
    int read_delay_ms;
    int preparation_delay_ms;
    int confirmation_delay_ms;
    int operation;
    int operation_limit;

    client_install_signal_handlers();
    if (!client_start("writer", options, &socket_fd, &database_size,
                      &stages, &default_operations, &read_delay_ms,
                      &preparation_delay_ms, &confirmation_delay_ms)) {
        return 1;
    }

    operation_limit = options->operations < 0 ? default_operations : options->operations;

    (void)database_size;
    (void)read_delay_ms;
    for (operation = 0;
         !client_should_stop() &&
         (operation_limit == 0 || operation < operation_limit);
         ++operation) {
        int stage;
        unsigned int seed = options->seed + (unsigned int)operation;

        (void)snprintf(command, sizeof(command), "BEGIN %u", seed);
        if (!client_request(socket_fd, command, response, sizeof(response))) {
            break;
        }
        if (strncmp(response, "OK BEGIN", 8) != 0) {
            printf("[Writer %d] %s\n", options->id, response);
            client_sleep_ms(100);
            --operation;
            continue;
        }

        for (stage = 0; stage < stages && !client_should_stop(); ++stage) {
            client_sleep_ms(preparation_delay_ms);
            (void)snprintf(command, sizeof(command), "STAGE %d", stage);
            if (!client_request(socket_fd, command, response, sizeof(response)) ||
                strncmp(response, "OK STAGE", 8) != 0) {
                printf("[Writer %d] %s\n", options->id, response);
                break;
            }
        }

        if (client_should_stop()) {
            (void)client_request(socket_fd, "ABORT", response, sizeof(response));
            break;
        }
        if (stage != stages) {
            continue;
        }

        client_sleep_ms(confirmation_delay_ms);
        if (!client_request(socket_fd, "PUBLISH", response, sizeof(response))) {
            break;
        }
        printf("[Writer %d] %s\n", options->id, response);
    }

    if (!client_should_stop()) {
        (void)client_request(socket_fd, "QUIT", response, sizeof(response));
    }
    close(socket_fd);
    return 0;
}
