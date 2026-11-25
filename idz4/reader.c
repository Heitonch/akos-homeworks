#include "client_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int run_reader(const ClientOptions *options)
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
    int exit_status = 0;
    uint32_t random_state;

    client_install_signal_handlers();
    if (!client_start("reader", options, &socket_fd, &database_size,
                      &stages, &default_operations, &read_delay_ms,
                      &preparation_delay_ms, &confirmation_delay_ms)) {
        return 1;
    }

    operation_limit = options->operations < 0 ? default_operations : options->operations;
    random_state = options->seed ^ ((uint32_t)options->id * 0x9E3779B9U);
    if (random_state == 0U) random_state = 0xA341316CU;

    (void)stages;
    (void)preparation_delay_ms;
    (void)confirmation_delay_ms;

    for (operation = 0;
         !client_should_stop() &&
         (operation_limit == 0 || operation < operation_limit);
         ++operation) {
        uint32_t bound = (uint32_t)database_size;
        uint32_t threshold = (uint32_t)(0U - bound) % bound;
        unsigned int index;

        do {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 17;
            random_state ^= random_state << 5;
        } while (random_state < threshold);
        index = random_state % bound;
        client_sleep_ms(read_delay_ms);
        if (client_should_stop()) break;
        (void)snprintf(command, sizeof(command), "READ %u", index);
        if (!client_request(socket_fd, command, response, sizeof(response))) {
            if (!client_should_stop()) {
                fprintf(stderr, "[Reader %d] ошибка обмена с сервером\n", options->id);
                exit_status = 1;
            }
            break;
        }
        if (strncmp(response, "DATA ", 5) != 0) {
            fprintf(stderr, "[Reader %d] сервер отклонил чтение: %s\n", options->id, response);
            exit_status = 1;
            break;
        }
        printf("[Reader %d] %s\n", options->id, response);
    }

    if (!client_should_stop() && exit_status == 0) {
        if (!client_request(socket_fd, "QUIT", response, sizeof(response)) ||
            strcmp(response, "BYE") != 0) {
            exit_status = 1;
        }
    }
    printf("[Reader %d] завершил работу (%s)\n", options->id,
           exit_status == 0 ? "успешно" : "с ошибкой");
    close(socket_fd);
    return exit_status;
}
