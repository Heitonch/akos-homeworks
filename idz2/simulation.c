#define _POSIX_C_SOURCE 200809L
#include "simulation.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct Sim {
    Db *db;
    const Config *c;
    pthread_mutex_t mu;
    pthread_mutex_t io;
    pthread_cond_t changed;
    sem_t *slot;
    int writing;
    atomic_int stop;
    atomic_int finished;
    atomic_int failed;
};

static volatile sig_atomic_t interrupted = 0;

static void on_sigint(int sig) {
    (void)sig;
    interrupted = 1;
}

static void say(Sim *s, const char *fmt, ...) {
    pthread_mutex_lock(&s->io);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&s->io);
}

static void sleep_ms(Sim *s, int ms) {
    while (ms > 0 && !atomic_load(&s->stop)) {
        int part = ms < 50 ? ms : 50;
        struct timespec t = {part / 1000, (long)(part % 1000) * 1000000L};
        while (nanosleep(&t, &t) == -1 && errno == EINTR) {
            if (atomic_load(&s->stop)) return;
        }
        ms -= part;
    }
}

static int cmp(const void *x, const void *y) {
    int a = *(const int *)x, b = *(const int *)y;
    return (a > b) - (a < b);
}

static void print_db(const Db *db) {
    printf("[");
    for (int i = 0; i < db->n; i++) printf("%s%d", i ? " " : "", db->a[i]);
    printf("]");
}

static unsigned int next_rand(unsigned int *seed) {
    unsigned int x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *seed = x;
}

static int rnd(unsigned int *seed, int min, int max) {
    unsigned long long range = (unsigned long long)((long long)max - min + 1);
    return (int)(min + (long long)((unsigned long long)next_rand(seed) % range));
}

static void finish(int *left) {
    if (*left > 0) (*left)--;
}

static void cancel_all(Sim *s) {
    atomic_store(&s->failed, 1);
    atomic_store(&s->stop, 1);
    pthread_mutex_lock(&s->mu);
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mu);
}

static void *reader_thread(void *arg) {
    Reader *r = arg;
    Sim *s = r->s;
    for (; r->left != 0 && !atomic_load(&s->stop); finish(&r->left)) {
        pthread_mutex_lock(&s->mu);
        while (s->c->strategy == WAIT_NEW && s->writing && !atomic_load(&s->stop)) {
            say(s, "\n[Reader %d] Waiting for version %d\n", r->id, s->db->ver + 1);
            pthread_cond_wait(&s->changed, &s->mu);
        }
        if (atomic_load(&s->stop)) {
            pthread_mutex_unlock(&s->mu);
            break;
        }
        int ver = s->db->ver;
        int i = rnd(&r->seed, 0, s->db->n - 1);
        int val = s->db->a[i];
        int old = s->writing;
        pthread_mutex_unlock(&s->mu);
        if (old) say(s, "\n[Reader %d] Version %d is being prepared, reading confirmed version %d\n", r->id, ver + 1, ver);
        say(s, "\n[Reader %d]\nVersion: %d\nIndex: %d, value: %d, record: %d, product: %lld\n", r->id, ver, i, val, i + 1, (long long)val * (i + 1));
        sleep_ms(s, s->c->read_ms);
        say(s, "Reader %d finished version %d\n", r->id, ver);
        r->done++;
    }
    atomic_fetch_add(&s->finished, 1);
    return NULL;
}

static int start_write(Writer *w) {
    Sim *s = w->s;
    w->buf = malloc((size_t)s->db->n * sizeof(int));
    if (!w->buf) return 0;
    pthread_mutex_lock(&s->mu);
    if (atomic_load(&s->stop)) {
        pthread_mutex_unlock(&s->mu);
        free(w->buf);
        w->buf = NULL;
        return -1;
    }
    memcpy(w->buf, s->db->a, (size_t)s->db->n * sizeof(int));
    s->writing = 1;
    w->active = 1;
    w->stage = 0;
    say(s, "\n[Writer %d] Started preparing version %d\n", w->id, s->db->ver + 1);
    pthread_mutex_unlock(&s->mu);
    return 1;
}

static void write_stage(Writer *w, int ver) {
    Sim *s = w->s;
    long long n = s->db->n;
    int l = (int)((long long)w->stage * n / s->c->stages);
    int r = (int)((long long)(w->stage + 1) * n / s->c->stages);
    for (int i = l; i < r; i++) w->buf[i] = rnd(&w->seed, s->c->min, s->c->max);
    w->stage++;
    say(s, "\n[Writer %d] Stage %d/%d for version %d completed\n", w->id, w->stage, s->c->stages, ver);
    sleep_ms(s, s->c->prep_ms);
}

static int publish(Writer *w, int ver) {
    Sim *s = w->s;
    qsort(w->buf, (size_t)s->db->n, sizeof(int), cmp);
    say(s, "\n[Writer %d] Checking version %d...\n", w->id, ver);
    for (int i = 0; i < s->db->n; i++) {
        if (w->buf[i] <= 0 || (i && w->buf[i - 1] > w->buf[i])) return 0;
    }
    say(s, "[Writer %d] Check: OK\n", w->id);
    sleep_ms(s, s->c->confirm_ms);
    pthread_mutex_lock(&s->mu);
    say(s, "[Writer %d] Version %d confirmed\n", w->id, ver);
    memcpy(s->db->a, w->buf, (size_t)s->db->n * sizeof(int));
    s->db->ver = ver;
    s->writing = 0;
    pthread_mutex_lock(&s->io);
    printf("[Writer %d] Version %d published\nDatabase: ", w->id, ver);
    print_db(s->db);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&s->io);
    pthread_cond_broadcast(&s->changed);
    pthread_mutex_unlock(&s->mu);
    free(w->buf);
    w->buf = NULL;
    w->active = 0;
    w->stage = 0;
    finish(&w->left);
    w->done++;
    return 1;
}

static void *writer_thread(void *arg) {
    Writer *w = arg;
    Sim *s = w->s;
    while (w->left != 0 && !atomic_load(&s->stop)) {
        int rc;
        do { rc = sem_wait(s->slot); } while (rc == -1 && errno == EINTR && !atomic_load(&s->stop));
        if (rc != 0) {
            if (!atomic_load(&s->stop)) cancel_all(s);
            break;
        }
        int started = start_write(w);
        if (started <= 0) {
            if (started == 0) cancel_all(s);
            sem_post(s->slot);
            break;
        }
        int ver;
        pthread_mutex_lock(&s->mu);
        ver = s->db->ver + 1;
        pthread_mutex_unlock(&s->mu);
        while (w->stage < s->c->stages) write_stage(w, ver);
        if (!publish(w, ver)) {
            cancel_all(s);
            sem_post(s->slot);
            break;
        }
        sem_post(s->slot);
    }
    free(w->buf);
    w->buf = NULL;
    atomic_fetch_add(&s->finished, 1);
    return NULL;
}

int run(Db *db, const Config *c) {
    Sim s = {.db = db, .c = c};
    Reader *r = calloc((size_t)c->readers, sizeof(Reader));
    Writer *w = calloc((size_t)c->writers, sizeof(Writer));
    pthread_t *rt = calloc((size_t)c->readers, sizeof(pthread_t));
    pthread_t *wt = calloc((size_t)c->writers, sizeof(pthread_t));
    if (!r || !w || !rt || !wt) {
        free(r); free(w); free(rt); free(wt);
        return 0;
    }
    if (pthread_mutex_init(&s.mu, NULL) != 0) goto fail_alloc;
    if (pthread_mutex_init(&s.io, NULL) != 0) goto fail_mu;
    if (pthread_cond_init(&s.changed, NULL) != 0) goto fail_io;
    atomic_init(&s.stop, 0);
    atomic_init(&s.failed, 0);
    atomic_init(&s.finished, 0);
    char name[64];
    snprintf(name, sizeof(name), "/akos_idz2_%ld", (long)getpid());
    sem_unlink(name);
    s.slot = sem_open(name, O_CREAT | O_EXCL, 0600, 1);
    if (s.slot == SEM_FAILED) {
        perror("sem_open");
        goto fail_cond;
    }
    sem_unlink(name);
    struct sigaction action = {0}, prev = {0};
    action.sa_handler = on_sigint;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &prev);
    interrupted = 0;
    int left = c->ops ? c->ops : -1;
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    for (int i = 0; i < c->readers; i++) {
        r[i].id = i + 1;
        r[i].left = left;
        r[i].s = &s;
        r[i].seed = (seed ^ (0x9e3779b9u * (unsigned)(i + 1))) | 1u;
    }
    for (int i = 0; i < c->writers; i++) {
        w[i].id = i + 1;
        w[i].left = left;
        w[i].s = &s;
        w[i].seed = (seed ^ (0x85ebca6bu * (unsigned)(i + 1))) | 1u;
    }
    say(&s, "\n==============================\nSimulation started\nVersion: %d\n", db->ver);
    pthread_mutex_lock(&s.io);
    printf("Database: ");
    print_db(db);
    printf("\nStrategy: %s\nMode: %s\n", c->strategy == READ_OLD ? "READ_OLD" : "WAIT_NEW", c->ops ? "finite" : "unlimited");
    if (!c->ops) printf("Press Ctrl+C to stop\n");
    printf("==============================\n");
    fflush(stdout);
    pthread_mutex_unlock(&s.io);
    int nr = 0, nw = 0;
    for (int i = 0; i < c->writers; i++) {
        if (pthread_create(&wt[i], NULL, writer_thread, &w[i]) != 0) goto launch_failed;
        nw++;
    }
    for (int i = 0; i < c->readers; i++) {
        if (pthread_create(&rt[i], NULL, reader_thread, &r[i]) != 0) goto launch_failed;
        nr++;
    }
    while (!interrupted && atomic_load(&s.finished) < nr + nw && !atomic_load(&s.failed)) {
        struct timespec t = {0, 20000000L};
        nanosleep(&t, NULL);
    }
    if (interrupted || atomic_load(&s.failed)) {
        atomic_store(&s.stop, 1);
        pthread_mutex_lock(&s.mu);
        pthread_cond_broadcast(&s.changed);
        pthread_mutex_unlock(&s.mu);
    }
    goto joined;
launch_failed:
    fprintf(stderr, "Failed to create threads\n");
    atomic_store(&s.failed, 1);
    atomic_store(&s.stop, 1);
    pthread_mutex_lock(&s.mu);
    pthread_cond_broadcast(&s.changed);
    pthread_mutex_unlock(&s.mu);
joined:
    for (int i = 0; i < nr; i++) pthread_join(rt[i], NULL);
    for (int i = 0; i < nw; i++) pthread_join(wt[i], NULL);
    long long reads = 0, writes = 0;
    for (int i = 0; i < c->readers; i++) reads += r[i].done;
    for (int i = 0; i < c->writers; i++) writes += w[i].done;
    say(&s, "\n==============================\n%s\nFinal version: %d\n", atomic_load(&s.failed) ? "Simulation failed" : interrupted ? "Stopped by user" : "Simulation finished", db->ver);
    pthread_mutex_lock(&s.io);
    printf("Final database: ");
    print_db(db);
    printf("\nReads: %lld\nPublications: %lld\n==============================\n", reads, writes);
    fflush(stdout);
    pthread_mutex_unlock(&s.io);
    int ok = !atomic_load(&s.failed);
    sigaction(SIGINT, &prev, NULL);
    sem_close(s.slot);
    pthread_cond_destroy(&s.changed);
    pthread_mutex_destroy(&s.io);
    pthread_mutex_destroy(&s.mu);
    free(r); free(w); free(rt); free(wt);
    return ok;
fail_cond:
    pthread_cond_destroy(&s.changed);
fail_io:
    pthread_mutex_destroy(&s.io);
fail_mu:
    pthread_mutex_destroy(&s.mu);
fail_alloc:
    free(r); free(w); free(rt); free(wt);
    return 0;
}
