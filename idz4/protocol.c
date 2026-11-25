#define _POSIX_C_SOURCE 200809L
#include "common.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>

int parse_integer(const char *text, long min_value, long max_value, long *value)
{
    char *end = NULL;
    long parsed;

    if (text == NULL || *text == '\0') {
        return 0;
    }

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < min_value || parsed > max_value) {
        return 0;
    }

    *value = parsed;
    return 1;
}

int send_line(int fd, const char *line)
{
    size_t length = strlen(line);
    size_t sent = 0;

    while (sent < length) {
        ssize_t result = send(fd, line + sent, length - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (result == 0) {
            return 0;
        }
        sent += (size_t)result;
    }

    while (sent < length + 1) {
        static const char newline = '\n';
        ssize_t result = send(fd, &newline, 1, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (result == 0) {
            return 0;
        }
        sent += (size_t)result;
    }

    return 1;
}

int receive_line(int fd, char *buffer, size_t capacity)
{
    size_t used = 0;

    if (capacity < 2) {
        return 0;
    }

    while (used + 1 < capacity) {
        char character;
        ssize_t result = recv(fd, &character, 1, 0);

        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 0;
        }
        if (result == 0) {
            return 0;
        }
        if (character == '\n') {
            buffer[used] = '\0';
            return 1;
        }
        if (character != '\r') {
            buffer[used++] = character;
        }
    }

    return 0;
}

int connect_to_server(const char *host, const char *port)
{
    struct sockaddr_in address;
    long port_number;
    int socket_fd;

    if (!parse_integer(port, 1, 65535, &port_number)) {
        fprintf(stderr, "Некорректный TCP-порт: %s\n", port);
        return -1;
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((unsigned short)port_number);

    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fprintf(stderr, "Некорректный IPv4-адрес: %s\n", host);
        close(socket_fd);
        return -1;
    }

    if (connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("connect");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}
