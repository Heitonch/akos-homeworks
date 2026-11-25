#include "client_internal.h"

#include <stdio.h>
#include <unistd.h>

int run_observer(const ClientOptions *options)
{
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
    if (!client_start("observer", options, &socket_fd, &database_size,
                      &stages, &default_operations, &read_delay_ms,
                      &preparation_delay_ms, &confirmation_delay_ms)) {
        return 1;
    }

    operation_limit = options->operations < 0 ? default_operations : options->operations;

    (void)database_size;
    (void)stages;
    (void)read_delay_ms;
    (void)preparation_delay_ms;
    (void)confirmation_delay_ms;

    for (operation = 0;
         !client_should_stop() &&
         (operation_limit == 0 || operation < operation_limit);
         ++operation) {
        if (!client_request(socket_fd, "STATUS", response, sizeof(response))) {
            break;
        }
        puts(response);
        if (operation_limit == 0 || operation + 1 < operation_limit) {
            client_sleep_ms(1000);
        }
    }

    if (!client_should_stop()) {
        (void)client_request(socket_fd, "QUIT", response, sizeof(response));
    }
    close(socket_fd);
    return 0;
}
