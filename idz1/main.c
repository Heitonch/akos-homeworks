#include "simulation.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static int get_int(const char *text, int min, int max) {
    char s[128];
    for (;;) {
        printf("%s", text);
        if (!fgets(s, sizeof(s), stdin)) exit(EXIT_FAILURE);
        errno = 0;
        char *end;
        long x = strtol(s, &end, 10);
        if (end == s) {
            printf("Enter a number from %d to %d.\n", min, max);
            continue;
        }
        while (*end == ' ' || *end == '\t') end++;
        if (!errno && (*end == '\n' || *end == '\0') && x >= min && x <= max) return (int)x;
        printf("Enter a number from %d to %d.\n", min, max);
    }
}

int main(void) {
    Db db;
    Config c;
    db.n = get_int("Database size: ", 1, 100000);
    db.a = malloc((size_t)db.n * sizeof(int));
    if (!db.a) return EXIT_FAILURE;
    printf("Enter %d positive numbers in nondecreasing order:\n", db.n);
    int min = 1;
    for (int i = 0; i < db.n; i++) {
        char text[32];
        snprintf(text, sizeof(text), "a[%d]: ", i);
        db.a[i] = get_int(text, min, INT_MAX);
        min = db.a[i];
    }

    db.ver = 1;
    c.readers = get_int("Readers: ", 1, 1000);
    c.writers = get_int("Writers: ", 1, 1000);
    c.ops = get_int("Operations per participant (0 = unlimited): ", 0, 1000000);
    c.min = get_int("Minimum new value: ", 1, INT_MAX);
    c.max = get_int("Maximum new value: ", c.min, INT_MAX);
    c.stages = get_int("Preparation stages: ", 1, 100000);
    c.strategy = get_int("Reader strategy (0 = read old, 1 = wait): ", 0, 1) ? WAIT_NEW : READ_OLD;
    c.read_ms = get_int("Read delay, ms: ", 0, 60000);
    c.prep_ms = get_int("Preparation delay, ms: ", 0, 60000);
    c.confirm_ms = get_int("Confirmation delay, ms: ", 0, 60000);

    int ok = run(&db, &c);
    free(db.a);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}