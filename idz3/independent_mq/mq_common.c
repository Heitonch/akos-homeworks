#define _POSIX_C_SOURCE 200809L
#include "mq_common.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>

static volatile sig_atomic_t interrupted = 0;
static pid_t active_server_pid = -1;
static void on_signal(int sig) { (void)sig; interrupted = 1; }

void mq_install_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static struct timespec deadline(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 1;
    return ts;
}

int mq_was_interrupted(void) { return interrupted != 0; }
void mq_set_server_pid(pid_t pid) { active_server_pid = pid; }

static int server_alive(void) {
    if (active_server_pid <= 0) return 1;
    if (kill(active_server_pid, 0) == -1 && errno == ESRCH) return 0;
    char path[64], state[256];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)active_server_pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, state, sizeof(state) - 1);
        close(fd);
        if (n > 0) {
            state[n] = '\0';
            char *end = strrchr(state, ')');
            if (end && end[1] == ' ' && (end[2] == 'Z' || end[2] == 'X')) return 0;
        }
    }
    return 1;
}

int mq_wait_semaphore(sem_t *sem) {
    mq_install_signals();
    while (!interrupted) {
        struct timespec until = deadline();
        if (sem_timedwait(sem, &until) == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) { if (!server_alive()) return 0; continue; }
        perror("sem_timedwait"); return 0;
    }
    return 0;
}

int mq_send_force(mqd_t queue, const Message *message) {
    for (;;) {
        struct timespec until = deadline();
        if (mq_timedsend(queue, (const char *)message, sizeof(*message), 0, &until) == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) { if (!server_alive()) return 0; continue; }
        return 0;
    }
}

int mq_send_wait(mqd_t queue, const Message *message) {
    mq_install_signals();
    while (!interrupted) {
        if (mq_timedsend(queue, (const char *)message, sizeof(*message), 0, &(struct timespec){.tv_sec = time(NULL) + 1, .tv_nsec = 0}) == 0) return 1;
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) { if (!server_alive()) return 0; continue; }
        perror("mq_send"); return 0;
    }
    return 0;
}

int mq_receive_wait(mqd_t queue, Message *message) {
    mq_install_signals();
    while (!interrupted) {
        struct timespec until = deadline();
        ssize_t n = mq_timedreceive(queue, (char *)message, sizeof(*message), NULL, &until);
        if (n >= 0) return n == (ssize_t)sizeof(*message);
        if (errno == EINTR) continue;
        if (errno == ETIMEDOUT) { if (!server_alive()) return 0; continue; }
        perror("mq_receive"); return 0;
    }
    return 0;
}

int mq_create_reply(char *name, size_t size, mqd_t *queue) {
    snprintf(name, size, "/akos3_reply_%ld_%ld", (long)getpid(), (long)time(NULL));
    struct mq_attr attr = {.mq_flags = 0, .mq_maxmsg = 10, .mq_msgsize = sizeof(Message), .mq_curmsgs = 0};
    *queue = mq_open(name, O_CREAT | O_EXCL | O_RDONLY, 0600, &attr);
    if (*queue == (mqd_t)-1) { perror("mq_open reply"); return 0; }
    return 1;
}

void mq_close_reply(const char *name, mqd_t queue) {
    if (queue != (mqd_t)-1) mq_close(queue);
    if (name && *name) mq_unlink(name);
}

int mq_client_open(mqd_t *server, char *reply_name, size_t reply_size, mqd_t *reply) {
    *server = mq_open(MQ_REQUEST_NAME, O_WRONLY);
    if (*server == (mqd_t)-1) { perror("mq_open: запустите mq_server"); return 0; }
    if (!mq_create_reply(reply_name, reply_size, reply)) { mq_close(*server); return 0; }
    return 1;
}

int mq_request(mqd_t server, mqd_t reply, const Message *request, Message *response) {
    if (!mq_send_wait(server, request)) return 0;
    return mq_receive_wait(reply, response);
}

int mq_sleep(int ms) {
    while (ms > 0 && !interrupted) {
        int part = ms > 50 ? 50 : ms;
        struct timespec ts = {part / 1000, (long)(part % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) == -1) {
            if (errno == EINTR) { if (interrupted) return 0; continue; }
            return 0;
        }
        ms -= part;
    }
    return !interrupted;
}

static void write_all(int fd, const char *s, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t x = write(fd, s + done, n - done);
        if (x > 0) done += (size_t)x;
        else if (x < 0 && errno == EINTR) continue;
        else break;
    }
}

void mq_log(sem_t *log_sem, const char *path, const char *fmt, ...) {
    char line[1200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n > sizeof(line) - 2) n = (int)sizeof(line) - 3;
    line[n++] = '\n';
    if (log_sem) while (sem_wait(log_sem) == -1 && errno == EINTR) {}
    write_all(STDOUT_FILENO, line, (size_t)n);
    if (path) {
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { write_all(fd, line, (size_t)n); close(fd); }
    }
    if (log_sem) sem_post(log_sem);
}

int mq_open_named_sems(sem_t **state, sem_t **writer, sem_t **log_sem, int create) {
    int flags = create ? O_CREAT | O_EXCL : 0;
    if (create) {
        *state = sem_open(MQ_STATE_SEM, flags, 0600, 1);
        if (*state == SEM_FAILED) return 0;
        *writer = sem_open(MQ_WRITER_SEM, flags, 0600, 1);
        if (*writer == SEM_FAILED) { sem_close(*state); sem_unlink(MQ_STATE_SEM); return 0; }
        *log_sem = sem_open(MQ_LOG_SEM, flags, 0600, 1);
        if (*log_sem == SEM_FAILED) {
            sem_close(*writer); sem_close(*state); sem_unlink(MQ_WRITER_SEM); sem_unlink(MQ_STATE_SEM); return 0;
        }
    } else {
        *state = sem_open(MQ_STATE_SEM, 0);
        if (*state == SEM_FAILED) return 0;
        *writer = sem_open(MQ_WRITER_SEM, 0);
        if (*writer == SEM_FAILED) { sem_close(*state); return 0; }
        *log_sem = sem_open(MQ_LOG_SEM, 0);
        if (*log_sem == SEM_FAILED) { sem_close(*writer); sem_close(*state); return 0; }
    }
    return 1;
}

void mq_close_named_sems(sem_t *state, sem_t *writer, sem_t *log_sem) {
    if (state && state != SEM_FAILED) sem_close(state);
    if (writer && writer != SEM_FAILED) sem_close(writer);
    if (log_sem && log_sem != SEM_FAILED) sem_close(log_sem);
}
