#define _POSIX_C_SOURCE 200809L
#include "mq_common.h"
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int send_abort(mqd_t server, int id) {
    Message abort_message = {.type = MSG_WRITE_ABORT, .pid = (int)getpid(), .id = id};
    return mq_send_force(server, &abort_message);
}

int main(int argc, char **argv) {
    int id = 1, ops = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) ops = atoi(argv[++i]);
        else { fprintf(stderr, "Использование: mq_writer [--id N] [--ops N]\n"); return 1; }
    }
    if (id < 1 || id > 256 || ops < -1 || ops > 1000000) return 1;
    mq_install_signals();
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
    // Семафор остаётся захваченным на протяжении одной полной публикации.
    sem_t *writer_sem = sem_open(MQ_WRITER_SEM, 0);
    if (writer_sem == SEM_FAILED) { perror("sem_open writer"); mq_close_reply(reply_name, reply); mq_close(server); return 1; }
    mq_set_server_pid((pid_t)cfg.server_pid);
    int count = ops >= 0 ? ops : cfg.ops;
    unsigned int seed = cfg.seed ^ (0x85ebca6bu * (unsigned int)id);
    if (!seed) seed = 1;
    dprintf(STDOUT_FILENO, "[MQ Writer %d] Подключён; operations=%s; stages=%d\n", id, count ? "finite" : "unlimited", cfg.stages);
    long long done = 0;
    while (!mq_was_interrupted() && (count == 0 || done < count)) {
        if (!mq_wait_semaphore(writer_sem)) break;
        if (mq_was_interrupted()) { sem_post(writer_sem); break; }
        Message begin = {.type = MSG_WRITE_BEGIN, .pid = (int)getpid(), .id = id, .seed = seed};
        snprintf(begin.reply, sizeof(begin.reply), "%s", reply_name);
        Message response;
        if (!mq_request(server, reply, &begin, &response)) {
            (void)send_abort(server, id);
            sem_post(writer_sem);
            break;
        }
        if (response.status) {
            sem_post(writer_sem);
            fprintf(stderr, "Сервер не начал подготовку версии.\n");
            break;
        }
        int version = response.version;
        dprintf(STDOUT_FILENO, "[MQ Writer %d] Подготовка версии %d\n", id, version);
        int complete = 1;
        for (int stage = 0; stage < cfg.stages; stage++) {
            Message step = {.type = MSG_WRITE_STAGE, .pid = (int)getpid(), .id = id, .stage = stage};
            snprintf(step.reply, sizeof(step.reply), "%s", reply_name);
            if (!mq_request(server, reply, &step, &response) || response.status) { complete = 0; break; }
            dprintf(STDOUT_FILENO, "[MQ Writer %d] Стадия %d/%d\n", id, stage + 1, cfg.stages);
            if (!mq_sleep(cfg.prep_ms)) { complete = 0; break; }
        }
        if (complete && !mq_sleep(cfg.confirm_ms)) complete = 0;
        if (!complete || mq_was_interrupted()) {
            (void)send_abort(server, id);
            sem_post(writer_sem);
            break;
        }
        Message publish = {.type = MSG_WRITE_PUBLISH, .pid = (int)getpid(), .id = id};
        snprintf(publish.reply, sizeof(publish.reply), "%s", reply_name);
        if (!mq_request(server, reply, &publish, &response) || response.status) {
            (void)send_abort(server, id);
            sem_post(writer_sem);
            fprintf(stderr, "Публикация не выполнена.\n");
            break;
        }
        sem_post(writer_sem);
        done++;
        dprintf(STDOUT_FILENO, "[MQ Writer %d] Версия %d опубликована\n", id, response.version);
        seed ^= 0x9e3779b9u;
    }
    dprintf(STDOUT_FILENO, "[MQ Writer %d] Завершено публикаций: %lld\n", id, done);
    sem_close(writer_sem);
    mq_close_reply(reply_name, reply);
    mq_close(server);
    return 0;
}
