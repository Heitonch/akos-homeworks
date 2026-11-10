#define _POSIX_C_SOURCE 200809L
#include "mq_common.h"
#include "../common/model.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted = 0;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

static void install_server_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int wait_sem(sem_t *sem) {
    while (sem_wait(sem) == -1) {
        if (errno == EINTR) { if (interrupted) return 0; continue; }
        return 0;
    }
    return 1;
}

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int valid(const int *a, int n) {
    for (int i = 0; i < n; i++) if (a[i] <= 0 || (i && a[i - 1] > a[i])) return 0;
    return n > 0;
}

// Отложенный запрос WAIT_NEW хранится до публикации или отмены текущей версии.
typedef struct {
    Message request;
} PendingRead;

typedef struct {
    Config cfg;
    int *buffers[2];
    int active;
    int version;
    int writing;
    int writer_pid;
    int writer_id;
    int write_version;
    int next_stage;
    unsigned int write_seed;
    long long reads;
    long long publications;
    long long stages;
    long long aborted;
    PendingRead pending[MQ_PENDING_MAX];
    int pending_count;
    mqd_t queue;
    sem_t *state_sem;
    sem_t *writer_sem;
    sem_t *log_sem;
    char log_path[IDZ3_PATH_LEN];
} Server;

static void send_response(Server *s, const Message *request, int status, int version,
                          int index, int value, long long product) {
    (void)s;
    if (!request->reply[0]) return;
    Message response = *request;
    response.type = MSG_RESPONSE;
    response.status = status;
    response.version = version;
    response.index = index;
    response.value = value;
    response.product = product;
    mqd_t reply = mq_open(request->reply, O_WRONLY);
    if (reply == (mqd_t)-1) return;
    (void)mq_send(reply, (const char *)&response, sizeof(response), 0);
    mq_close(reply);
}

static void answer_read(Server *s, const Message *request) {
    if (!wait_sem(s->state_sem)) return;
    int index = request->index;
    if (index < 0 || index >= s->cfg.n) index = 0;
    int value = s->buffers[s->active][index];
    int version = s->version;
    s->reads++;
    sem_post(s->state_sem);
    send_response(s, request, 0, version, index, value, (long long)value * (index + 1));
    mq_log(s->log_sem, s->log_path,
           "[Reader %d pid=%d] Read confirmed version=%d index=%d value=%d record=%d product=%lld",
           request->id, request->pid, version, index, value, index + 1, (long long)value * (index + 1));
}

static void flush_pending(Server *s) {
    int count = s->pending_count;
    s->pending_count = 0;
    for (int i = 0; i < count; i++) answer_read(s, &s->pending[i].request);
}

static void abort_write(Server *s, const Message *request, const char *reason) {
    if (s->writing && (!request || request->pid == s->writer_pid)) {
        s->writing = 0;
        s->aborted++;
        mq_log(s->log_sem, s->log_path, "[Writer %d pid=%d] %s; version %d remains active",
               s->writer_id, s->writer_pid, reason, s->version);
        if (request) send_response(s, request, 0, s->version, 0, 0, 0);
        flush_pending(s);
    }
}

// Сервер последовательно обрабатывает запросы; подготовка писателя разбита на отдельные сообщения.
static void handle_message(Server *s, const Message *m) {
    switch (m->type) {
        case MSG_HELLO: {
            Message response = *m;
            response.type = MSG_RESPONSE;
            response.status = 0;
            response.version = s->version;
            response.server_pid = (int)getpid();
            response.n = s->cfg.n;
            response.min_value = s->cfg.min_value;
            response.max_value = s->cfg.max_value;
            response.stages = s->cfg.stages;
            response.strategy = s->cfg.strategy;
            response.read_ms = s->cfg.read_ms;
            response.prep_ms = s->cfg.prep_ms;
            response.confirm_ms = s->cfg.confirm_ms;
            response.ops = s->cfg.ops;
            response.seed = s->cfg.seed;
            if (m->reply[0]) {
                mqd_t reply = mq_open(m->reply, O_WRONLY);
                if (reply != (mqd_t)-1) { (void)mq_send(reply, (const char *)&response, sizeof(response), 0); mq_close(reply); }
            }
            break;
        }
        case MSG_READ:
            if (s->cfg.strategy == WAIT_NEW && s->writing) {
                if (s->pending_count < MQ_PENDING_MAX) {
                    s->pending[s->pending_count++].request = *m;
                    mq_log(s->log_sem, s->log_path,
                           "[Reader %d pid=%d] Waiting for publication of version %d",
                           m->id, m->pid, s->version + 1);
                } else send_response(s, m, 1, s->version, 0, 0, 0);
            } else answer_read(s, m);
            break;

        case MSG_WRITE_BEGIN:
            if (s->writing) {
                send_response(s, m, 1, s->version, 0, 0, 0);
                break;
            }
            if (!wait_sem(s->state_sem)) break;
            memcpy(s->buffers[1 - s->active], s->buffers[s->active], (size_t)s->cfg.n * sizeof(int));
            s->writing = 1;
            s->writer_pid = m->pid;
            s->writer_id = m->id;
            s->write_version = s->version + 1;
            s->next_stage = 0;
            s->write_seed = m->seed ? m->seed : 1;
            sem_post(s->state_sem);
            mq_log(s->log_sem, s->log_path, "[Writer %d pid=%d] Started preparing version %d",
                   m->id, m->pid, s->write_version);
            send_response(s, m, 0, s->write_version, 0, 0, 0);
            break;

        case MSG_WRITE_STAGE:
            if (!s->writing || m->pid != s->writer_pid || m->stage != s->next_stage || m->stage >= s->cfg.stages) {
                send_response(s, m, 1, s->version, 0, 0, 0);
                break;
            }
            {
                int left = (int)((long long)m->stage * s->cfg.n / s->cfg.stages);
                int right = (int)((long long)(m->stage + 1) * s->cfg.n / s->cfg.stages);
                int *work = s->buffers[1 - s->active];
                for (int i = left; i < right; i++)
                    work[i] = random_between(&s->write_seed, s->cfg.min_value, s->cfg.max_value);
                s->next_stage++;
                s->stages++;
                mq_log(s->log_sem, s->log_path, "[Writer %d pid=%d] Stage %d/%d completed for version %d",
                       m->id, m->pid, m->stage + 1, s->cfg.stages, s->write_version);
                send_response(s, m, 0, s->write_version, 0, 0, 0);
            }
            break;

        case MSG_WRITE_PUBLISH:
            if (!s->writing || m->pid != s->writer_pid || s->next_stage != s->cfg.stages) {
                send_response(s, m, 1, s->version, 0, 0, 0);
                break;
            }
            qsort(s->buffers[1 - s->active], (size_t)s->cfg.n, sizeof(int), cmp_int);
            if (!valid(s->buffers[1 - s->active], s->cfg.n)) {
                int old_version = s->version;
                s->writing = 0;
                s->aborted++;
                mq_log(s->log_sem, s->log_path, "[Writer %d pid=%d] Validation failed; publication cancelled", m->id, m->pid);
                send_response(s, m, 1, old_version, 0, 0, 0);
                flush_pending(s);
                break;
            }
            if (!wait_sem(s->state_sem)) break;
            s->active = 1 - s->active;
            s->version = s->write_version;
            s->publications++;
            s->writing = 0;
            sem_post(s->state_sem);
            mq_log(s->log_sem, s->log_path, "[Writer %d pid=%d] Version %d validated and published",
                   m->id, m->pid, s->version);
            send_response(s, m, 0, s->version, 0, 0, 0);
            flush_pending(s);
            break;

        case MSG_WRITE_ABORT:
            abort_write(s, m, "Preparation interrupted and rolled back");
            break;

        case MSG_CANCEL:
            for (int i = 0; i < s->pending_count; i++) {
                if (s->pending[i].request.pid == m->pid) {
                    for (int j = i + 1; j < s->pending_count; j++) s->pending[j - 1] = s->pending[j];
                    s->pending_count--; i--;
                }
            }
            break;

        case MSG_STOP:
            abort_write(s, NULL, "Server is stopping; active preparation rolled back");
            s->writing = 0;
            flush_pending(s);
            interrupted = 1;
            break;

        default:
            break;
    }
}

// Сервер создаёт общую очередь запросов и именованные POSIX-семафоры.
static int start_server(Server *s) {
    struct mq_attr attr = {.mq_flags = 0, .mq_maxmsg = 10, .mq_msgsize = sizeof(Message), .mq_curmsgs = 0};
    s->queue = mq_open(MQ_REQUEST_NAME, O_CREAT | O_EXCL | O_RDONLY, 0600, &attr);
    if (s->queue == (mqd_t)-1) { perror("mq_open server queue (возможно, нужен mq_cleanup)"); return 0; }
    if (!mq_open_named_sems(&s->state_sem, &s->writer_sem, &s->log_sem, 1)) {
        perror("sem_open"); mq_close(s->queue); mq_unlink(MQ_REQUEST_NAME); return 0;
    }
    return 1;
}

static void cleanup_server(Server *s) {
    if (s->queue != (mqd_t)-1) { mq_close(s->queue); mq_unlink(MQ_REQUEST_NAME); }
    mq_close_named_sems(s->state_sem, s->writer_sem, s->log_sem);
}

int main(int argc, char **argv) {
    Config cfg;
    int *initial = NULL;
    const char *path = NULL;
    int rc;
    if (argc == 1) rc = config_interactive(&cfg, &initial);
    else rc = config_apply_args(argc, argv, &cfg, &initial, &path);
    if (rc == -1) return 0;
    if (!rc || !validate_database(initial, cfg.n)) {
        fprintf(stderr, "Некорректная конфигурация или начальная база.\n"); free(initial); return 1;
    }
    Server s;
    memset(&s, 0, sizeof(s));
    s.queue = (mqd_t)-1;
    s.cfg = cfg;
    s.version = 1;
    s.buffers[0] = malloc((size_t)cfg.n * sizeof(int));
    s.buffers[1] = malloc((size_t)cfg.n * sizeof(int));
    if (!s.buffers[0] || !s.buffers[1]) { perror("malloc"); free(initial); free(s.buffers[0]); free(s.buffers[1]); return 1; }
    memcpy(s.buffers[0], initial, (size_t)cfg.n * sizeof(int));
    memcpy(s.buffers[1], initial, (size_t)cfg.n * sizeof(int));
    free(initial);
    mkdir("logs", 0755);
    snprintf(s.log_path, sizeof(s.log_path), "logs/idz3_independent_mq.log");
    if (!start_server(&s)) { free(s.buffers[0]); free(s.buffers[1]); return 1; }
    install_server_signals();
    mq_log(s.log_sem, s.log_path, "[MQ SERVER] Сервер базы данных запущен; POSIX message queue=%s", MQ_REQUEST_NAME);
    mq_log(s.log_sem, s.log_path, "Версия=1; размер=%d; стратегия=%s; stages=%d; seed=%u",
           cfg.n, cfg.strategy == READ_OLD ? "READ_OLD" : "WAIT_NEW", cfg.stages, cfg.seed);
    mq_log(s.log_sem, s.log_path, "Ожидание независимых процессов mq_reader и mq_writer. Остановка: mq_stop.");

    while (!interrupted) {
        Message m;
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec++;
        ssize_t n = mq_timedreceive(s.queue, (char *)&m, sizeof(m), NULL, &until);
        if (n == (ssize_t)sizeof(m)) handle_message(&s, &m);
        else if (n == -1 && errno == EINTR) continue;
        else if (n == -1 && errno == ETIMEDOUT) continue;
        else if (n == -1) { perror("mq_receive"); break; }
    }
    abort_write(&s, NULL, "Server shutdown rolled back unfinished publication");
    flush_pending(&s);
    mq_log(s.log_sem, s.log_path, "[MQ SERVER] Завершение. version=%d reads=%lld publications=%lld stages=%lld aborted=%lld",
           s.version, s.reads, s.publications, s.stages, s.aborted);
    mq_log(s.log_sem, s.log_path, "Журнал: %s", s.log_path);
    cleanup_server(&s);
    free(s.buffers[0]); free(s.buffers[1]);
    return 0;
}
