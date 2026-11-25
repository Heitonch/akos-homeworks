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
    int exit_status = 0;

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
        int begin_ok = 0;
        unsigned int seed = options->seed + (unsigned int)operation;

        while (!client_should_stop()) {
            (void)snprintf(command, sizeof(command), "BEGIN %u", seed);
            if (!client_request(socket_fd, command, response, sizeof(response))) {
                if (!client_should_stop()) {
                    fprintf(stderr, "[Writer %d] ошибка при начале подготовки\n", options->id);
                    exit_status = 1;
                }
                break;
            }
            if (strncmp(response, "OK BEGIN ", 9) == 0) {
                begin_ok = 1;
                break;
            }
            if (strcmp(response, "ERR busy") != 0) {
                fprintf(stderr, "[Writer %d] сервер отклонил BEGIN: %s\n",
                        options->id, response);
                exit_status = 1;
                break;
            }
            printf("[Writer %d] другой писатель выполняет подготовку; ожидание\n",
                   options->id);
            client_sleep_ms(100);
        }
        if (!begin_ok) break;

        for (stage = 0; stage < stages && !client_should_stop(); ++stage) {
            client_sleep_ms(preparation_delay_ms);
            if (client_should_stop()) break;
            (void)snprintf(command, sizeof(command), "STAGE %d", stage);
            if (!client_request(socket_fd, command, response, sizeof(response))) {
                if (!client_should_stop()) {
                    fprintf(stderr, "[Writer %d] ошибка обмена на стадии %d\n",
                            options->id, stage + 1);
                    exit_status = 1;
                }
                break;
            }
            if (strncmp(response, "OK STAGE ", 9) != 0) {
                fprintf(stderr, "[Writer %d] стадия %d отклонена: %s\n",
                        options->id, stage + 1, response);
                exit_status = 1;
                break;
            }
        }

        if (client_should_stop()) {
            (void)client_request(socket_fd, "ABORT", response, sizeof(response));
            break;
        }
        if (stage != stages) {
            if (exit_status == 0) exit_status = 1;
            break;
        }

        client_sleep_ms(confirmation_delay_ms);
        if (client_should_stop()) {
            (void)client_request(socket_fd, "ABORT", response, sizeof(response));
            break;
        }
        if (!client_request(socket_fd, "PUBLISH", response, sizeof(response))) {
            if (!client_should_stop()) {
                fprintf(stderr, "[Writer %d] ошибка при публикации версии\n", options->id);
                exit_status = 1;
            }
            break;
        }
        if (strncmp(response, "OK PUBLISHED ", 13) != 0) {
            fprintf(stderr, "[Writer %d] публикация отклонена: %s\n",
                    options->id, response);
            exit_status = 1;
            break;
        }
        printf("[Writer %d] %s\n", options->id, response);
    }

    if (!client_should_stop() && exit_status == 0) {
        if (!client_request(socket_fd, "QUIT", response, sizeof(response)) ||
            strcmp(response, "BYE") != 0) {
            exit_status = 1;
        }
    }
    printf("[Writer %d] завершил работу (%s)\n", options->id,
           exit_status == 0 ? "успешно" : "с ошибкой");
    close(socket_fd);
    return exit_status;
}
