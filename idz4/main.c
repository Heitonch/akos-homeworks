#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *program)
{
    fprintf(stderr,
        "Использование:\n"
        "  %s server [параметры сервера]\n"
        "  %s reader|writer|observer [--host IPv4] [--port PORT] [--id N] [--ops N] [--seed N]\n",
        program, program);
}

static int parse_client_options(int argc, char **argv, ClientOptions *options)
{
    int index;

    *options = (ClientOptions){
        .host = "127.0.0.1",
        .port = "5050",
        .id = 1,
        .operations = -1,
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

        if (strcmp(argument, "--host") == 0) {
            options->host = value;
        } else if (strcmp(argument, "--port") == 0) {
            if (!parse_integer(value, 1, 65535, &parsed)) return 0;
            options->port = value;
        } else if (strcmp(argument, "--id") == 0) {
            if (!parse_integer(value, 0, 2147483647L, &parsed)) return 0;
            options->id = (int)parsed;
        } else if (strcmp(argument, "--ops") == 0) {
            if (!parse_integer(value, 0, 1000000000L, &parsed)) return 0;
            options->operations = (int)parsed;
        } else if (strcmp(argument, "--seed") == 0) {
            if (!parse_integer(value, 0, 4294967295L, &parsed)) return 0;
            options->seed = (unsigned int)parsed;
        } else {
            fprintf(stderr, "Неизвестный параметр клиента: %s\n", argument);
            return 0;
        }
    }

    return 1;
}

int main(int argc, char **argv)
{
    ClientOptions options;

    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "server") == 0) {
        return run_server(argc - 1, argv + 1);
    }
    if (strcmp(argv[1], "reader") != 0 && strcmp(argv[1], "writer") != 0 &&
        strcmp(argv[1], "observer") != 0) {
        print_usage(argv[0]);
        return 2;
    }
    if (!parse_client_options(argc - 1, argv + 1, &options)) {
        print_usage(argv[0]);
        return 2;
    }

    if (strcmp(argv[1], "reader") == 0) {
        return run_reader(&options);
    }
    if (strcmp(argv[1], "writer") == 0) {
        return run_writer(&options);
    }
    return run_observer(&options);
}
