#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define DEFAULT_PORT "5050"
#define DEFAULT_SIZE 5
#define DEFAULT_STAGES 3
#define DEFAULT_OPERATIONS 5
#define DEFAULT_MIN_VALUE 1
#define DEFAULT_MAX_VALUE 100
#define DEFAULT_DELAY_MS 100

typedef struct {
    int fd;
    int id;
    char role[16];
    char input[IDZ4_LINE_SIZE];
    size_t input_length;
    int closing;
} Client;

typedef struct {
    int client_slot;
    int record_index;
} WaitingRead;

typedef struct {
    const char *bind_address;
    const char *port;
    ReadStrategy strategy;
    int size;
    int stages;
    int operations;
    int min_value;
    int max_value;
    int read_delay_ms;
    int preparation_delay_ms;
    int confirmation_delay_ms;
    unsigned int seed;
} ServerOptions;

typedef struct {
    int buffers[2][IDZ4_MAX_SIZE];
    int active_buffer;
    int version;
    int writing;
    int writer_slot;
    int writer_id;
    int next_stage;
    int pending_version;
    unsigned int random_state;
    unsigned long long reads;
    unsigned long long publications;
    unsigned long long completed_stages;
    unsigned long long aborted_writes;
    WaitingRead waiting[IDZ4_MAX_WAITERS];
    size_t waiting_count;
} Database;

static volatile sig_atomic_t stop_requested;
static FILE *log_stream;

static void handle_signal(int signal_number)
{
    (void)signal_number;
    stop_requested = 1;
}

static void log_event(const char *format, ...)
{
    va_list arguments;
    va_list copy;

    va_start(arguments, format);
    va_copy(copy, arguments);
    vprintf(format, arguments);
    putchar('\n');
    if (log_stream != NULL) {
        vfprintf(log_stream, format, copy);
        fputc('\n', log_stream);
        fflush(log_stream);
    }
    va_end(copy);
    va_end(arguments);
}

static int send_response(int fd, const char *format, ...)
{
    char message[IDZ4_LINE_SIZE];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    return send_line(fd, message);
}

static unsigned int next_random(Database *database)
{
    database->random_state = database->random_state * 1664525U + 1013904223U;
    return database->random_state;
}

static int compare_integers(const void *left, const void *right)
{
    int first = *(const int *)left;
    int second = *(const int *)right;
    return (first > second) - (first < second);
}

static int validate_options(const ServerOptions *options)
{
    if (options->size < 1 || options->size > IDZ4_MAX_SIZE ||
        options->stages < 1 || options->stages > options->size ||
        options->operations < 0 || options->min_value < 1 ||
        options->max_value < options->min_value ||
        options->read_delay_ms < 0 || options->preparation_delay_ms < 0 ||
        options->confirmation_delay_ms < 0) {
        fprintf(stderr, "Некорректные параметры сервера\n");
        return 0;
    }
    if (options->size >= 5 && options->max_value < 19) {
        fprintf(stderr, "--max должен быть не меньше максимального начального значения (19)\n");
        return 0;
    }
    return 1;
}

static int parse_server_options(int argc, char **argv, ServerOptions *options)
{
    int index;

    *options = (ServerOptions){
        .bind_address = "0.0.0.0",
        .port = DEFAULT_PORT,
        .strategy = STRATEGY_READ_OLD,
        .size = DEFAULT_SIZE,
        .stages = DEFAULT_STAGES,
        .operations = DEFAULT_OPERATIONS,
        .min_value = DEFAULT_MIN_VALUE,
        .max_value = DEFAULT_MAX_VALUE,
        .read_delay_ms = DEFAULT_DELAY_MS,
        .preparation_delay_ms = DEFAULT_DELAY_MS,
        .confirmation_delay_ms = DEFAULT_DELAY_MS,
        .seed = 42U
    };

    for (index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        const char *value;
        long parsed;

        if (index + 1 >= argc) {
            fprintf(stderr, "Для параметра %s требуется значение\n", argument);
            return 0;
        }
        value = argv[++index];

        if (strcmp(argument, "--bind") == 0) {
            options->bind_address = value;
        } else if (strcmp(argument, "--port") == 0) {
            if (!parse_integer(value, 1, 65535, &parsed)) return 0;
            options->port = value;
        } else if (strcmp(argument, "--strategy") == 0) {
            if (strcmp(value, "READ_OLD") == 0) options->strategy = STRATEGY_READ_OLD;
            else if (strcmp(value, "WAIT_NEW") == 0) options->strategy = STRATEGY_WAIT_NEW;
            else return 0;
        } else if (strcmp(argument, "--ops") == 0) {
            if (!parse_integer(value, 0, 1000000000L, &parsed)) return 0;
            options->operations = (int)parsed;
        } else if (strcmp(argument, "--stages") == 0) {
            if (!parse_integer(value, 1, IDZ4_MAX_SIZE, &parsed)) return 0;
            options->stages = (int)parsed;
        } else if (strcmp(argument, "--size") == 0) {
            if (!parse_integer(value, 1, IDZ4_MAX_SIZE, &parsed)) return 0;
            options->size = (int)parsed;
        } else if (strcmp(argument, "--min") == 0) {
            if (!parse_integer(value, 1, 2147483647L, &parsed)) return 0;
            options->min_value = (int)parsed;
        } else if (strcmp(argument, "--max") == 0) {
            if (!parse_integer(value, 1, 2147483647L, &parsed)) return 0;
            options->max_value = (int)parsed;
        } else if (strcmp(argument, "--seed") == 0) {
            if (!parse_integer(value, 0, 4294967295L, &parsed)) return 0;
            options->seed = (unsigned int)parsed;
        } else if (strcmp(argument, "--read-ms") == 0) {
            if (!parse_integer(value, 0, 3600000, &parsed)) return 0;
            options->read_delay_ms = (int)parsed;
        } else if (strcmp(argument, "--prep-ms") == 0) {
            if (!parse_integer(value, 0, 3600000, &parsed)) return 0;
            options->preparation_delay_ms = (int)parsed;
        } else if (strcmp(argument, "--confirm-ms") == 0) {
            if (!parse_integer(value, 0, 3600000, &parsed)) return 0;
            options->confirmation_delay_ms = (int)parsed;
        } else {
            fprintf(stderr, "Неизвестный параметр сервера: %s\n", argument);
            return 0;
        }
    }

    return validate_options(options);
}

static void initialize_database(Database *database, const ServerOptions *options)
{
    static const int initial_values[] = {2, 5, 8, 12, 19};
    int index;

    memset(database, 0, sizeof(*database));
    database->version = 1;
    database->writer_slot = -1;
    database->random_state = options->seed == 0 ? 1U : options->seed;

    for (index = 0; index < options->size; ++index) {
        int value = index < 5 ? initial_values[index] : 20 + index;
        database->buffers[0][index] = value;
        database->buffers[1][index] = value;
    }
}

static void respond_to_read(Database *database, const ServerOptions *options,
                            Client *client, int record_index)
{
    (void)options;
    int value;
    int version;
    long long product;

    value = database->buffers[database->active_buffer][record_index];
    version = database->version;
    product = (long long)value * (record_index + 1);
    ++database->reads;

    (void)send_response(client->fd, "DATA %d %d %d %d %lld",
                        version, record_index, value, record_index + 1, product);
    log_event("[Reader %d] версия=%d индекс=%d значение=%d запись=%d произведение=%lld",
              client->id, version, record_index, value, record_index + 1, product);
}

static void flush_waiting_reads(Database *database, const ServerOptions *options,
                                Client *clients)
{
    size_t index;

    for (index = 0; index < database->waiting_count; ++index) {
        int slot = database->waiting[index].client_slot;
        if (slot >= 0 && slot < IDZ4_MAX_CLIENTS && clients[slot].fd >= 0) {
            respond_to_read(database, options, &clients[slot],
                            database->waiting[index].record_index);
        }
    }
    database->waiting_count = 0;
}

static void abort_write(Database *database, const ServerOptions *options,
                        Client *clients, const char *reason)
{
    if (!database->writing) {
        return;
    }

    log_event("[Writer %d] %s; версия %d сохранена",
              database->writer_id, reason, database->version);
    database->writing = 0;
    database->writer_slot = -1;
    ++database->aborted_writes;
    flush_waiting_reads(database, options, clients);
}

static void remove_client_waits(Database *database, int slot)
{
    size_t source;
    size_t destination = 0;

    for (source = 0; source < database->waiting_count; ++source) {
        if (database->waiting[source].client_slot != slot) {
            database->waiting[destination++] = database->waiting[source];
        }
    }
    database->waiting_count = destination;
}

static int parse_command_integer(const char *line, const char *prefix,
                                 long minimum, long maximum, long *value)
{
    size_t prefix_length = strlen(prefix);
    if (strncmp(line, prefix, prefix_length) != 0) {
        return 0;
    }
    return parse_integer(line + prefix_length, minimum, maximum, value);
}

static int parse_command_seed(const char *line, unsigned int *seed)
{
    long parsed;
    if (!parse_command_integer(line, "BEGIN ", 0, 4294967295L, &parsed)) {
        return 0;
    }
    *seed = (unsigned int)parsed;
    return 1;
}

static int register_client(Client *client, const char *line,
                           const ServerOptions *options, const Database *database)
{
    char role[16];
    int identifier;
    char extra;

    if (sscanf(line, "HELLO %15s %d %c", role, &identifier, &extra) != 2 || identifier < 0 ||
        (strcmp(role, "reader") != 0 && strcmp(role, "writer") != 0 &&
         strcmp(role, "observer") != 0)) {
        (void)send_response(client->fd, "ERR handshake");
        close(client->fd);
        memset(client, 0, sizeof(*client));
        client->fd = -1;
        return 0;
    }

    (void)snprintf(client->role, sizeof(client->role), "%s", role);
    client->id = identifier;
    (void)send_response(client->fd, "WELCOME %d %d %d %d %d %d %d %d",
                        database->version, options->size,
                        options->strategy == STRATEGY_WAIT_NEW,
                        options->stages, options->operations,
                        options->read_delay_ms, options->preparation_delay_ms,
                        options->confirmation_delay_ms);
    log_event("[TCP] подключён клиент %s #%d", client->role, client->id);
    return 1;
}

static void process_command(Database *database, const ServerOptions *options,
                            Client *clients, int slot, const char *line)
{
    Client *client = &clients[slot];
    long parsed_index;
    unsigned int seed;

    if (client->role[0] == '\0') {
        if (!register_client(client, line, options, database)) {
            client->fd = -1;
        }
        return;
    }

    if (strncmp(line, "READ ", 5) == 0) {
        if (!parse_command_integer(line, "READ ", 0, options->size - 1, &parsed_index) ||
            strcmp(client->role, "reader") != 0) {
            (void)send_response(client->fd, "ERR index-or-role");
            return;
        }
        if (options->strategy == STRATEGY_WAIT_NEW && database->writing) {
            if (database->waiting_count == IDZ4_MAX_WAITERS) {
                (void)send_response(client->fd, "ERR wait-queue-full");
                return;
            }
            database->waiting[database->waiting_count++] =
                (WaitingRead){.client_slot = slot, .record_index = (int)parsed_index};
            log_event("[Reader %d] ожидает публикации версии %d",
                      client->id, database->version + 1);
        } else {
            respond_to_read(database, options, client, (int)parsed_index);
        }
        return;
    }

    if (strncmp(line, "BEGIN ", 6) == 0 && parse_command_seed(line, &seed)) {
        if (strcmp(client->role, "writer") != 0) {
            (void)send_response(client->fd, "ERR role");
        } else if (database->writing) {
            (void)send_response(client->fd, "ERR busy");
        } else {
            database->writing = 1;
            database->writer_slot = slot;
            database->writer_id = client->id;
            database->pending_version = database->version + 1;
            database->next_stage = 0;
            database->random_state = seed == 0 ? 1U : seed;
            memcpy(database->buffers[1 - database->active_buffer],
                   database->buffers[database->active_buffer],
                   (size_t)options->size * sizeof(int));
            log_event("[Writer %d] начал подготовку версии %d",
                      client->id, database->pending_version);
            (void)send_response(client->fd, "OK BEGIN %d", database->pending_version);
        }
        return;
    }

    if (strncmp(line, "STAGE ", 6) == 0) {
        if (!parse_command_integer(line, "STAGE ", 0, options->stages - 1, &parsed_index) ||
            !database->writing || database->writer_slot != slot ||
            parsed_index != database->next_stage) {
            (void)send_response(client->fd, "ERR stage");
            return;
        }
        {
            int stage_index = (int)parsed_index;
            int first = (int)((long long)stage_index * options->size / options->stages);
            int last = (int)((long long)(stage_index + 1) * options->size / options->stages);
            int *working = database->buffers[1 - database->active_buffer];
            int position;

            for (position = first; position < last; ++position) {
                unsigned int range = (unsigned int)(options->max_value - options->min_value) + 1U;
                working[position] = options->min_value + (int)(next_random(database) % range);
            }
        }
        ++database->next_stage;
        ++database->completed_stages;
        log_event("[Writer %d] завершил стадию %d/%d",
                  database->writer_id, (int)parsed_index + 1, options->stages);
        (void)send_response(client->fd, "OK STAGE %ld", parsed_index);
        return;
    }

    if (strcmp(line, "PUBLISH") == 0) {
        int *working = database->buffers[1 - database->active_buffer];
        int position;

        if (!database->writing || database->writer_slot != slot ||
            database->next_stage != options->stages) {
            (void)send_response(client->fd, "ERR incomplete");
            return;
        }

        qsort(working, (size_t)options->size, sizeof(int), compare_integers);
        for (position = 0; position < options->size; ++position) {
            if (working[position] < options->min_value ||
                working[position] > options->max_value ||
                (position > 0 && working[position - 1] > working[position])) {
                (void)send_response(client->fd, "ERR validation");
                abort_write(database, options, clients, "проверка не пройдена");
                return;
            }
        }

        database->active_buffer = 1 - database->active_buffer;
        database->version = database->pending_version;
        ++database->publications;
        database->writing = 0;
        database->writer_slot = -1;
        log_event("[Writer %d] версия %d проверена и опубликована",
                  database->writer_id, database->version);
        (void)send_response(client->fd, "OK PUBLISHED %d", database->version);
        flush_waiting_reads(database, options, clients);
        return;
    }

    if (strcmp(line, "ABORT") == 0) {
        if (database->writing && database->writer_slot == slot) {
            (void)send_response(client->fd, "OK ABORT");
            abort_write(database, options, clients, "подготовка отменена");
        } else {
            (void)send_response(client->fd, "ERR no-write");
        }
        return;
    }

    if (strcmp(line, "STATUS") == 0 && strcmp(client->role, "observer") == 0) {
        (void)send_response(client->fd,
            "STATUS version=%d size=%d writing=%d writer=%d reads=%llu publications=%llu stages=%llu aborted=%llu strategy=%s",
            database->version, options->size, database->writing,
            database->writing ? database->writer_id : -1, database->reads, database->publications,
            database->completed_stages, database->aborted_writes,
            options->strategy == STRATEGY_WAIT_NEW ? "WAIT_NEW" : "READ_OLD");
        return;
    }

    if (strcmp(line, "QUIT") == 0) {
        (void)send_response(client->fd, "BYE");
        log_event("[%s %d] завершил работу", client->role, client->id);
        client->closing = 1;
        return;
    }

    (void)send_response(client->fd, "ERR command");
}

static int create_listener(const ServerOptions *options)
{
    struct sockaddr_in address;
    long port_number;
    int socket_fd;
    int reuse_address = 1;

    if (!parse_integer(options->port, 1, 65535, &port_number)) {
        fprintf(stderr, "Некорректный TCP-порт\n");
        return -1;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address));

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port_number);
    if (inet_pton(AF_INET, options->bind_address, &address.sin_addr) != 1) {
        fprintf(stderr, "Некорректный IPv4-адрес: %s\n", options->bind_address);
        close(socket_fd);
        return -1;
    }

    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(socket_fd, 32) < 0) {
        perror("bind/listen");
        close(socket_fd);
        return -1;
    }
    return socket_fd;
}

static void close_client(Database *database, const ServerOptions *options,
                         Client *clients, int slot)
{
    int descriptor = clients[slot].fd;

    if (descriptor < 0) {
        return;
    }
    if (database->writing && database->writer_slot == slot) {
        abort_write(database, options, clients, "соединение писателя потеряно");
    }
    remove_client_waits(database, slot);
    close(descriptor);
    memset(&clients[slot], 0, sizeof(clients[slot]));
    clients[slot].fd = -1;
}

int run_server(int argc, char **argv)
{
    ServerOptions options;
    Database database;
    Client clients[IDZ4_MAX_CLIENTS];
    int listener;
    int index;

    if (!parse_server_options(argc, argv, &options)) {
        fprintf(stderr,
            "Использование: server [--bind IPv4] [--port PORT] [--strategy READ_OLD|WAIT_NEW] [--ops N] [--stages N] [--size N] [--min N] [--max N] [--seed N] [--read-ms N] [--prep-ms N] [--confirm-ms N]\n");
        return 2;
    }

    initialize_database(&database, &options);
    for (index = 0; index < IDZ4_MAX_CLIENTS; ++index) {
        clients[index].fd = -1;
    }

    listener = create_listener(&options);
    if (listener < 0) {
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);
    (void)mkdir("logs", 0755);
    log_stream = fopen("logs/idz4_server.log", "a");
    log_event("[SERVER] TCP %s:%s; size=%d strategy=%s",
              options.bind_address, options.port, options.size,
              options.strategy == STRATEGY_WAIT_NEW ? "WAIT_NEW" : "READ_OLD");

    while (!stop_requested) {
        struct pollfd descriptors[IDZ4_MAX_CLIENTS + 1];
        int client_slots[IDZ4_MAX_CLIENTS + 1];
        nfds_t descriptor_count = 1;
        int poll_result;

        descriptors[0] = (struct pollfd){.fd = listener, .events = POLLIN};
        client_slots[0] = -1;
        for (index = 0; index < IDZ4_MAX_CLIENTS; ++index) {
            if (clients[index].fd >= 0) {
                descriptors[descriptor_count] =
                    (struct pollfd){.fd = clients[index].fd, .events = POLLIN};
                client_slots[descriptor_count] = index;
                ++descriptor_count;
            }
        }

        poll_result = poll(descriptors, descriptor_count, 500);
        if (poll_result < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }
        if (descriptors[0].revents & POLLIN) {
            int accepted_fd = accept(listener, NULL, NULL);
            if (accepted_fd >= 0) {
                int free_slot = -1;
                for (index = 0; index < IDZ4_MAX_CLIENTS; ++index) {
                    if (clients[index].fd < 0) {
                        free_slot = index;
                        break;
                    }
                }
                if (free_slot < 0) {
                    close(accepted_fd);
                } else {
                    memset(&clients[free_slot], 0, sizeof(clients[free_slot]));
                    clients[free_slot].fd = accepted_fd;
                }
            }
        }

        for (nfds_t position = 1; position < descriptor_count; ++position) {
            int slot = client_slots[position];
            Client *client;
            char incoming[256];
            ssize_t received;
            size_t byte;

            if (slot < 0 || clients[slot].fd < 0 ||
                !(descriptors[position].revents & (POLLIN | POLLHUP | POLLERR))) {
                continue;
            }
            client = &clients[slot];
            received = recv(client->fd, incoming, sizeof(incoming), 0);
            if (received <= 0) {
                close_client(&database, &options, clients, slot);
                continue;
            }

            for (byte = 0; byte < (size_t)received && clients[slot].fd >= 0; ++byte) {
                char character = incoming[byte];
                if (character == '\n') {
                    client->input[client->input_length] = '\0';
                    process_command(&database, &options, clients, slot, client->input);
                    client->input_length = 0;
                    if (clients[slot].closing) {
                        close_client(&database, &options, clients, slot);
                        break;
                    }
                } else if (character != '\r' &&
                           client->input_length + 1 < sizeof(client->input)) {
                    client->input[client->input_length++] = character;
                } else {
                    close_client(&database, &options, clients, slot);
                }
            }
        }
    }

    abort_write(&database, &options, clients, "сервер завершает работу");
    log_event("[SERVER] stop version=%d reads=%llu publications=%llu stages=%llu aborted=%llu",
              database.version, database.reads, database.publications,
              database.completed_stages, database.aborted_writes);
    for (index = 0; index < IDZ4_MAX_CLIENTS; ++index) {
        if (clients[index].fd >= 0) close(clients[index].fd);
    }
    close(listener);
    if (log_stream != NULL) fclose(log_stream);
    return 0;
}
