#ifndef IDZ4_CLIENT_INTERNAL_H
#define IDZ4_CLIENT_INTERNAL_H

#include "common.h"

int client_start(const char *role, const ClientOptions *options,
                 int *socket_fd, int *database_size, int *stages,
                 int *default_operations, int *read_delay_ms,
                 int *preparation_delay_ms, int *confirmation_delay_ms);
int client_request(int socket_fd, const char *command, char *response,
                   size_t response_capacity);
int client_should_stop(void);
void client_install_signal_handlers(void);
void client_sleep_ms(int milliseconds);

#endif
