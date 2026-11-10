#define _POSIX_C_SOURCE 200809L
#include "model.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Обработчик сигнала только выставляет флаг; очистка IPC выполняется обычным кодом.
static volatile sig_atomic_t got_signal = 0;

static void handle_signal(int signo) {
    (void)signo;
    got_signal = 1;
}

void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

int interrupt_requested(void) { return got_signal != 0; }

size_t shared_size(int n) {
    return sizeof(Shared) + (size_t)2 * (size_t)n * sizeof(int);
}

int *shared_buffer(Shared *sh, int index) {
    return sh->data + (size_t)index * (size_t)sh->cfg.n;
}

static void make_sem_name(char *out, size_t size, const char *prefix, const char *suffix) {
    snprintf(out, size, "/%s_%s", prefix, suffix);
}

// Варианты 1 и 2 используют одну схему, но разный способ размещения семафоров.
int semset_init(Shared *sh, SemSet *ss, int named, const char *prefix) {
    memset(ss, 0, sizeof(*ss));
    ss->named = named;
    if (!named) {
        if (sem_init(&sh->local_sems[0], 1, 1) == -1) return 0;
        if (sem_init(&sh->local_sems[1], 1, 1) == -1) { sem_destroy(&sh->local_sems[0]); return 0; }
        if (sem_init(&sh->local_sems[2], 1, 0) == -1) { sem_destroy(&sh->local_sems[1]); sem_destroy(&sh->local_sems[0]); return 0; }
        if (sem_init(&sh->local_sems[3], 1, 1) == -1) {
            sem_destroy(&sh->local_sems[2]); sem_destroy(&sh->local_sems[1]); sem_destroy(&sh->local_sems[0]); return 0;
        }
        ss->state = &sh->local_sems[0]; ss->writer = &sh->local_sems[1];
        ss->changed = &sh->local_sems[2]; ss->log = &sh->local_sems[3];
        return 1;
    }
    const char *suffixes[4] = {"state", "writer", "changed", "log"};
    unsigned int values[4] = {1, 1, 0, 1};
    sem_t **slots[4] = {&ss->state, &ss->writer, &ss->changed, &ss->log};
    for (int i = 0; i < 4; i++) {
        make_sem_name(sh->sem_names[i], sizeof(sh->sem_names[i]), prefix, suffixes[i]);
        *slots[i] = sem_open(sh->sem_names[i], O_CREAT | O_EXCL, 0600, values[i]);
        if (*slots[i] == SEM_FAILED) {
            *slots[i] = NULL;
            for (int j = 0; j < i; j++) { sem_close(*slots[j]); sem_unlink(sh->sem_names[j]); *slots[j] = NULL; }
            return 0;
        }
    }
    return 1;
}

int semset_attach(Shared *sh, SemSet *ss, int named) {
    memset(ss, 0, sizeof(*ss));
    ss->named = named;
    if (!named) {
        ss->state = &sh->local_sems[0]; ss->writer = &sh->local_sems[1];
        ss->changed = &sh->local_sems[2]; ss->log = &sh->local_sems[3];
        return 1;
    }
    sem_t **slots[4] = {&ss->state, &ss->writer, &ss->changed, &ss->log};
    for (int i = 0; i < 4; i++) {
        *slots[i] = sem_open(sh->sem_names[i], 0);
        if (*slots[i] == SEM_FAILED) {
            *slots[i] = NULL;
            semset_close(ss);
            return 0;
        }
    }
    return 1;
}

void semset_close(SemSet *ss) {
    if (!ss || !ss->named) return;
    sem_t *items[4] = {ss->state, ss->writer, ss->changed, ss->log};
    for (int i = 0; i < 4; i++) if (items[i]) sem_close(items[i]);
    memset(ss, 0, sizeof(*ss));
}

void semset_destroy(Shared *sh, SemSet *ss) {
    if (ss->named) {
        sem_t *items[4] = {ss->state, ss->writer, ss->changed, ss->log};
        for (int i = 0; i < 4; i++) {
            if (items[i]) sem_close(items[i]);
            if (sh->sem_names[i][0]) sem_unlink(sh->sem_names[i]);
        }
    } else {
        for (int i = 0; i < 4; i++) sem_destroy(&sh->local_sems[i]);
    }
    memset(ss, 0, sizeof(*ss));
}

int sem_wait_safe(sem_t *sem) {
    for (;;) {
        if (sem_wait(sem) == 0) return 0;
        if (errno == EINTR) {
            if (got_signal) return -1;
            continue;
        }
        return -1;
    }
}

static int sem_wait_force(sem_t *sem) {
    while (sem_wait(sem) == -1) if (errno != EINTR) return -1;
    return 0;
}

int sem_post_many(sem_t *sem, int count) {
    if (count < 0) count = 0;
    if (count > 30000) count = 30000;
    for (int i = 0; i < count; i++) if (sem_post(sem) == -1) return 0;
    return 1;
}

static void write_all(int fd, const char *data, size_t len) {
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, data + done, len - done);
        if (n > 0) done += (size_t)n;
        else if (n < 0 && errno == EINTR) continue;
        else break;
    }
}

void log_event(Shared *sh, SemSet *ss, const char *fmt, ...) {
    char line[IDZ3_LOG_LINE];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(line) - 2) n = (int)sizeof(line) - 3;
    line[n++] = '\n';
    line[n] = '\0';
    if (ss && ss->log && sem_wait_force(ss->log) == 0) {
        write_all(STDOUT_FILENO, line, (size_t)n);
        if (sh && sh->log_path[0]) {
            int fd = open(sh->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { write_all(fd, line, (size_t)n); close(fd); }
        }
        sem_post(ss->log);
    } else {
        write_all(STDOUT_FILENO, line, (size_t)n);
    }
}

static void wake_readers(Shared *sh, SemSet *ss) {
    int count = sh->waiting_readers;
    if (count < 1) count = 1;
    sem_post_many(ss->changed, count);
}

void request_stop(Shared *sh, SemSet *ss) {
    if (!sh || !ss || !ss->state) return;
    if (sem_wait_force(ss->state) == 0) {
        sh->stop = 1;
        wake_readers(sh, ss);
        sem_post_many(ss->writer, sh->cfg.writers + 1);
        sem_post(ss->state);
    }
}

int is_stopped(Shared *sh, SemSet *ss) {
    int value = 1;
    if (sem_wait_force(ss->state) == 0) {
        value = sh->stop;
        sem_post(ss->state);
    }
    return value || got_signal;
}

int delay_ms(Shared *sh, SemSet *ss, int ms) {
    while (ms > 0) {
        if (got_signal) { request_stop(sh, ss); return 0; }
        if (is_stopped(sh, ss)) return 0;
        int part = ms > 50 ? 50 : ms;
        struct timespec ts = {part / 1000, (long)(part % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) == -1) {
            if (errno == EINTR) {
                if (got_signal) { request_stop(sh, ss); return 0; }
                continue;
            }
            return 0;
        }
        ms -= part;
    }
    return !is_stopped(sh, ss);
}

unsigned int random_next(unsigned int *seed) {
    unsigned int x = *seed;
    if (x == 0) x = 0x6d2b79f5u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *seed = x;
    return x;
}

int random_between(unsigned int *seed, int low, int high) {
    unsigned long long range = (unsigned long long)((long long)high - (long long)low) + 1ULL;
    return (int)((long long)low + (long long)(random_next(seed) % range));
}

static int compare_ints(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

int validate_database(const int *data, int n) {
    if (!data || n < 1) return 0;
    for (int i = 0; i < n; i++) if (data[i] <= 0 || (i && data[i - 1] > data[i])) return 0;
    return 1;
}

void print_database(const int *data, int n) {
    char out[900];
    size_t used = 0;
    used += (size_t)snprintf(out + used, sizeof(out) - used, "[");
    int shown = n < 32 ? n : 32;
    for (int i = 0; i < shown && used < sizeof(out) - 32; i++)
        used += (size_t)snprintf(out + used, sizeof(out) - used, "%s%d", i ? " " : "", data[i]);
    if (shown < n) used += (size_t)snprintf(out + used, sizeof(out) - used, " ... (%d elements)", n);
    (void)snprintf(out + used, sizeof(out) - used, "]");
    write_all(STDOUT_FILENO, out, strlen(out));
}

static void finish_worker(Shared *sh, SemSet *ss) {
    if (sem_wait_force(ss->state) == 0) {
        if (sh->active_workers > 0) sh->active_workers--;
        sem_post(ss->state);
    }
}

// Читатель копирует нужное значение под блокировкой и после этого не зависит от смены буфера.
static int reader_step(Shared *sh, SemSet *ss, int id, unsigned int *seed) {
    int version, index, value;
    for (;;) {
        if (sem_wait_safe(ss->state) == -1) { request_stop(sh, ss); return 0; }
        if (sh->stop) { sem_post(ss->state); return 0; }
        if (sh->cfg.strategy == WAIT_NEW && sh->writing) {
            sh->waiting_readers++;
            int waiting_for = sh->version + 1;
            sem_post(ss->state);
            log_event(sh, ss, "[Reader %d] Waiting for publication of version %d", id, waiting_for);
            if (sem_wait_safe(ss->changed) == -1) request_stop(sh, ss);
            if (sem_wait_force(ss->state) == 0) {
                if (sh->waiting_readers > 0) sh->waiting_readers--;
                sem_post(ss->state);
            }
            continue;
        }
        int *data = shared_buffer(sh, sh->active_buffer);
        index = (int)(random_next(seed) % (unsigned int)sh->cfg.n);
        value = data[index];
        version = sh->version;
        sh->reads++;
        int writing = sh->writing;
        sem_post(ss->state);
        if (writing)
            log_event(sh, ss, "[Reader %d] Version %d is being prepared; reading confirmed version %d", id, version + 1, version);
        log_event(sh, ss, "[Reader %d] Read version=%d index=%d value=%d record=%d product=%lld",
                  id, version, index, value, index + 1, (long long)value * (long long)(index + 1));
        return 1;
    }
}

// Писатель меняет только неактивный буфер; публикация сводится к переключению active_buffer.
static int writer_operation(Shared *sh, SemSet *ss, int id, unsigned int *seed) {
    if (sem_wait_safe(ss->writer) == -1) { request_stop(sh, ss); return 0; }
    if (sem_wait_safe(ss->state) == -1) { sem_post(ss->writer); request_stop(sh, ss); return 0; }
    if (sh->stop) {
        sem_post(ss->state); sem_post(ss->writer); return 0;
    }
    int source = sh->active_buffer;
    int target = 1 - source;
    int n = sh->cfg.n;
    int version = sh->version + 1;
    memcpy(shared_buffer(sh, target), shared_buffer(sh, source), (size_t)n * sizeof(int));
    sh->writing = 1;
    sem_post(ss->state);
    log_event(sh, ss, "[Writer %d] Started preparing version %d", id, version);

    int *work = shared_buffer(sh, target);
    for (int stage = 0; stage < sh->cfg.stages; stage++) {
        if (is_stopped(sh, ss)) break;
        int left = (int)((long long)stage * n / sh->cfg.stages);
        int right = (int)((long long)(stage + 1) * n / sh->cfg.stages);
        for (int i = left; i < right; i++) work[i] = random_between(seed, sh->cfg.min_value, sh->cfg.max_value);
        if (sem_wait_force(ss->state) == 0) {
            sh->stages_done++;
            sem_post(ss->state);
        }
        log_event(sh, ss, "[Writer %d] Stage %d/%d completed for version %d", id, stage + 1, sh->cfg.stages, version);
        if (!delay_ms(sh, ss, sh->cfg.prep_ms)) break;
    }

    if (is_stopped(sh, ss)) {
        if (sem_wait_force(ss->state) == 0) {
            sh->writing = 0;
            sh->aborted_writes++;
            wake_readers(sh, ss);
            sem_post(ss->state);
        }
        log_event(sh, ss, "[Writer %d] Preparation of version %d was cancelled; version %d remains active", id, version, sh->version);
        sem_post(ss->writer);
        return 0;
    }

    qsort(work, (size_t)n, sizeof(int), compare_ints);
    log_event(sh, ss, "[Writer %d] Checking version %d", id, version);
    if (!validate_database(work, n)) {
        if (sem_wait_force(ss->state) == 0) {
            sh->failed = 1; sh->stop = 1; sh->writing = 0; sh->aborted_writes++;
            wake_readers(sh, ss);
            sem_post(ss->state);
        }
        log_event(sh, ss, "[Writer %d] Validation failed; publication was cancelled", id);
        sem_post(ss->writer);
        return 0;
    }
    log_event(sh, ss, "[Writer %d] Validation passed", id);
    if (!delay_ms(sh, ss, sh->cfg.confirm_ms)) {
        if (sem_wait_force(ss->state) == 0) {
            sh->writing = 0; sh->aborted_writes++; wake_readers(sh, ss); sem_post(ss->state);
        }
        log_event(sh, ss, "[Writer %d] Confirmation interrupted; previous version preserved", id);
        sem_post(ss->writer);
        return 0;
    }

    int waiting = 0;
    int did_publish = 0;
    if (sem_wait_force(ss->state) == 0) {
        if (sh->stop) {
            sh->writing = 0; sh->aborted_writes++;
        } else {
            sh->active_buffer = target;
            sh->version = version;
            sh->publications++;
            sh->writing = 0;
            did_publish = 1;
        }
        waiting = sh->waiting_readers;
        sem_post(ss->state);
    }
    sem_post_many(ss->changed, waiting);
    if (!did_publish) {
        log_event(sh, ss, "[Writer %d] Publication of version %d was cancelled", id, version);
        sem_post(ss->writer);
        return 0;
    }
    log_event(sh, ss, "[Writer %d] Version %d confirmed and published", id, version);
    if (sem_wait_force(ss->state) == 0) {
        char line[IDZ3_LOG_LINE];
        size_t used = (size_t)snprintf(line, sizeof(line), "[Writer %d] Database: [", id);
        int *active = shared_buffer(sh, sh->active_buffer);
        int shown = n < 24 ? n : 24;
        for (int i = 0; i < shown && used < sizeof(line) - 32; i++)
            used += (size_t)snprintf(line + used, sizeof(line) - used, "%s%d", i ? " " : "", active[i]);
        if (shown < n) used += (size_t)snprintf(line + used, sizeof(line) - used, " ... (%d elements)", n);
        (void)snprintf(line + used, sizeof(line) - used, "]");
        sem_post(ss->state);
        log_event(sh, ss, "%s", line);
    }
    sem_post(ss->writer);
    return 1;
}

// Точка входа для одного процесса-читателя или процесса-писателя.
int run_worker(Shared *sh, SemSet *ss, WorkerRole role, int id, int ops_override) {
    install_signal_handlers();
    if (sem_wait_force(ss->state) == -1) return 0;
    sh->active_workers++;
    sem_post(ss->state);
    unsigned int seed = sh->cfg.seed ^ (0x9e3779b9u * (unsigned int)id) ^
                       (role == ROLE_WRITER ? 0x85ebca6bu : 0x27d4eb2du);
    if (!seed) seed = 1;
    int ops = ops_override >= 0 ? ops_override : sh->cfg.ops;
    long long completed = 0;
    log_event(sh, ss, "[%s %d] Process %ld started; operations=%s", role == ROLE_READER ? "Reader" : "Writer", id,
              (long)getpid(), ops ? "finite" : "unlimited");
    while ((ops == 0 || completed < ops) && !is_stopped(sh, ss)) {
        if (role == ROLE_READER) {
            if (!reader_step(sh, ss, id, &seed)) break;
            completed++;
            if (!delay_ms(sh, ss, sh->cfg.read_ms)) break;
        } else {
            if (!writer_operation(sh, ss, id, &seed)) break;
            completed++;
        }
    }
    log_event(sh, ss, "[%s %d] Process %ld finished after %lld operation(s)",
              role == ROLE_READER ? "Reader" : "Writer", id, (long)getpid(), completed);
    finish_worker(sh, ss);
    return !sh->failed;
}
