#define _POSIX_C_SOURCE 200809L
#include "mq_common.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned int next_random(unsigned int *seed) {
    unsigned int x = *seed ? *seed : 1u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *seed = x;
}

int main(int argc, char **argv) {
    int id = 1, ops = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) ops = atoi(argv[++i]);
        else { fprintf(stderr, "Использование: mq_reader [--id N] [--ops N]\n"); return 1; }
    }
    if (id < 1 || id > 256 || ops < -1 || ops > 1000000) return 1;
    mq_install_signals();
    // Для каждого клиента создаётся отдельная очередь, поэтому ответы не смешиваются.
    mqd_t server, reply;
    char reply_name[MQ_REPLY_LEN];
    if (!mq_client_open(&server, reply_name, sizeof(reply_name), &reply)) return 1;
    Message hello = {.type = MSG_HELLO, .pid = (int)getpid(), .id = id};
    snprintf(hello.reply, sizeof(hello.reply), "%s", reply_name);
    Message cfg;
    if (!mq_request(server, reply, &hello, &cfg) || cfg.status) {
        fprintf(stderr, "Не удалось получить настройки сервера.\n");
        mq_close_reply(reply_name, reply); mq_close(server); return 1;
    }
    mq_set_server_pid((pid_t)cfg.server_pid);
    int count = ops >= 0 ? ops : cfg.ops;
    unsigned int seed = cfg.seed ^ (0x9e3779b9u * (unsigned int)id);
    dprintf(STDOUT_FILENO, "[MQ Reader %d] Подключён: database size=%d, strategy=%s, operations=%s\n",
            id, cfg.n, cfg.strategy == 0 ? "READ_OLD" : "WAIT_NEW", count ? "finite" : "unlimited");
    long long done = 0;
    while (!mq_was_interrupted() && (count == 0 || done < count)) {
        Message request = {.type = MSG_READ, .pid = (int)getpid(), .id = id,
                           .index = (int)(next_random(&seed) % (unsigned int)cfg.n)};
        snprintf(request.reply, sizeof(request.reply), "%s", reply_name);
        Message response;
        if (!mq_request(server, reply, &request, &response)) {
            Message cancel = {.type = MSG_CANCEL, .pid = (int)getpid(), .id = id};
            (void)mq_send_force(server, &cancel);
            break;
        }
        if (response.status) { fprintf(stderr, "Сервер отклонил чтение.\n"); break; }
        dprintf(STDOUT_FILENO, "[MQ Reader %d] version=%d index=%d value=%d record=%d product=%lld\n",
                id, response.version, response.index, response.value, response.index + 1, response.product);
        done++;
        if (!mq_sleep(cfg.read_ms)) break;
    }
    dprintf(STDOUT_FILENO, "[MQ Reader %d] Завершено чтений: %lld\n", id, done);
    mq_close_reply(reply_name, reply);
    mq_close(server);
    return 0;
}
