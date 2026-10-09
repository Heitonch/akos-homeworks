#define _POSIX_C_SOURCE 200809L

#include "simulation.h"
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t stop = 0;

static void on_sigint(int sig) {
    (void)sig;
    stop = 1;
}

static void sleep_ms(int ms) {
    if (ms <= 0 || stop) return;
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR && !stop) {}
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

static int rnd(int min, int max) {
    unsigned long long range = (unsigned long long)((long long)max - min + 1);
    return min + (int)((unsigned long long)rand() % range);
}

static void finish(int *left) {
    if (*left > 0) (*left)--;
}

static void read_step(Reader *r, const Db *db, const Config *c) {
    int i = rand() % db->n;
    int val = db->a[i];

    printf("\n[Reader %d]\n", r->id);
    printf("Version: %d\n", db->ver);
    printf("Index: %d, value: %d, record: %d, product: %lld\n", i, val, i + 1, (long long)val * (i + 1));

    sleep_ms(c->read_ms);
    printf("Reader %d finished version %d\n", r->id, db->ver);

    finish(&r->left);
    r->done++;
}

static int start_write(Writer *w, const Db *db) {
    w->buf = malloc((size_t)db->n * sizeof(int));
    if (!w->buf) return 0;

    memcpy(w->buf, db->a, (size_t)db->n * sizeof(int));
    w->active = 1;
    w->stage = 0;

    printf("\n[Writer %d]\nStarted preparing version %d\n", w->id, db->ver + 1);
    return 1;
}

static void write_stage(Writer *w, const Db *db, const Config *c) {
    int l = w->stage * db->n / c->stages;
    int r = (w->stage + 1) * db->n / c->stages;

    for (int i = l; i < r; i++) w->buf[i] = rnd(c->min, c->max);

    w->stage++;
    printf("\n[Writer %d]\nStage %d/%d for version %d completed\n", w->id, w->stage, c->stages, db->ver + 1);
    sleep_ms(c->prep_ms);
}

static int publish(Writer *w, Db *db, const Config *c) {
    int ver = db->ver + 1;

    qsort(w->buf, (size_t)db->n, sizeof(int), cmp);
    printf("\n[Writer %d]\nChecking version %d...\n", w->id, ver);

    for (int i = 0; i < db->n; i++) {
        if (w->buf[i] <= 0 || (i && w->buf[i - 1] > w->buf[i])) return 0;
    }

    printf("Check: OK\n");
    sleep_ms(c->confirm_ms);
    printf("Version %d confirmed\n", ver);

    memcpy(db->a, w->buf, (size_t)db->n * sizeof(int));
    db->ver = ver;

    printf("Version %d published\nDatabase: ", db->ver);
    print_db(db);
    printf("\n");

    free(w->buf);
    w->buf = NULL;
    w->active = 0;
    w->stage = 0;

    finish(&w->left);
    w->done++;
    return 1;
}

static int reader_idx(const Reader *r, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) if (r[i].left != 0) count++;
    if (!count) return -1;

    int k = rand() % count;
    for (int i = 0; i < n; i++) if (r[i].left != 0 && k-- == 0) return i;
    return -1;
}

static int writer_idx(const Writer *w, int n) {
    int count = 0;
    for (int i = 0; i < n; i++) if (!w[i].active && w[i].left != 0) count++;
    if (!count) return -1;

    int k = rand() % count;
    for (int i = 0; i < n; i++) if (!w[i].active && w[i].left != 0 && k-- == 0) return i;
    return -1;
}

static int readers_left(const Reader *r, int n) {
    for (int i = 0; i < n; i++) if (r[i].left != 0) return 1;
    return 0;
}

static int writers_left(const Writer *w, int n) {
    for (int i = 0; i < n; i++) if (w[i].active || w[i].left != 0) return 1;
    return 0;
}

int run(Db *db, const Config *c) {
    Reader *r = calloc((size_t)c->readers, sizeof(Reader));
    Writer *w = calloc((size_t)c->writers, sizeof(Writer));

    if (!r || !w) {
        free(r);
        free(w);
        return 0;
    }

    int left = c->ops ? c->ops : -1;

    for (int i = 0; i < c->readers; i++) {
        r[i].id = i + 1;
        r[i].left = left;
    }

    for (int i = 0; i < c->writers; i++) {
        w[i].id = i + 1;
        w[i].left = left;
    }

    signal(SIGINT, on_sigint);
    srand((unsigned)time(NULL));

    printf("\n==============================\n");
    printf("Simulation started\n");
    printf("Version: %d\nDatabase: ", db->ver);
    print_db(db);
    printf("\nStrategy: %s\n", c->strategy == READ_OLD ? "READ_OLD" : "WAIT_NEW");
    if (!c->ops) printf("Press Ctrl+C to stop\n");
    printf("==============================\n");

    int active = -1;
    int reader_turn = 0;

    while (!stop && (readers_left(r, c->readers) || writers_left(w, c->writers))) {
        if (active >= 0) {
            Writer *cur = &w[active];

            if (c->strategy == READ_OLD && reader_turn && readers_left(r, c->readers)) {
                int i = reader_idx(r, c->readers);
                printf("\nVersion %d is being prepared. Reader %d uses confirmed version %d\n", db->ver + 1, r[i].id, db->ver);
                read_step(&r[i], db, c);
                reader_turn = 0;
                continue;
            }

            if (c->strategy == WAIT_NEW && readers_left(r, c->readers)) printf("\nReaders are waiting for version %d\n", db->ver + 1);

            if (cur->stage < c->stages) {
                write_stage(cur, db, c);
                reader_turn = 1;
            } else {
                if (!publish(cur, db, c)) goto fail;
                active = -1;
                reader_turn = 0;
            }
            continue;
        }

        int have_r = readers_left(r, c->readers);
        int have_w = writers_left(w, c->writers);

        if (have_r && (!have_w || rand() % 2 == 0)) {
            int i = reader_idx(r, c->readers);
            read_step(&r[i], db, c);
        } else if (have_w) {
            int i = writer_idx(w, c->writers);
            if (i >= 0) {
                if (!start_write(&w[i], db)) goto fail;
                active = i;
            }
        }
    }

    if (stop && active >= 0) {
        Writer *cur = &w[active];
        while (cur->stage < c->stages) write_stage(cur, db, c);
        if (!publish(cur, db, c)) goto fail;
    }

    long long reads = 0, writes = 0;

    for (int i = 0; i < c->readers; i++) reads += r[i].done;
    for (int i = 0; i < c->writers; i++) writes += w[i].done;

    printf("\n==============================\n");
    printf("%s\n", stop ? "Stopped by user" : "Simulation finished");
    printf("Final version: %d\nFinal database: ", db->ver);
    print_db(db);
    printf("\nReads: %lld\nPublications: %lld\n", reads, writes);
    printf("==============================\n");

    for (int i = 0; i < c->writers; i++) free(w[i].buf);
    free(r);
    free(w);
    return 1;

fail:
    for (int i = 0; i < c->writers; i++) free(w[i].buf);
    free(r);
    free(w);
    return 0;
}