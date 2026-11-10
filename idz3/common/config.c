#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_line(const char *prompt, char *buf, size_t size) {
    size_t used = 0;
    if (prompt) (void)write(STDOUT_FILENO, prompt, strlen(prompt));
    while (used + 1 < size) {
        char ch;
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n == 0) return used == 0 ? 0 : (buf[used] = '\0', 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ch == '\n') break;
        if (ch != '\r') buf[used++] = ch;
    }
    buf[used] = '\0';
    return 1;
}

static int parse_long(const char *s, long min, long max, long *out) {
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    while (isspace((unsigned char)*end)) end++;
    if (errno || end == s || *end || v < min || v > max) return 0;
    *out = v;
    return 1;
}

void config_defaults(Config *c) {
    memset(c, 0, sizeof(*c));
    c->n = 5;
    c->readers = 2;
    c->writers = 1;
    c->ops = 2;
    c->min_value = 1;
    c->max_value = 20;
    c->stages = 3;
    c->strategy = READ_OLD;
    c->read_ms = 0;
    c->prep_ms = 20;
    c->confirm_ms = 0;
    c->seed = 20261110u;
}

static int parse_initial(const char *s, int n, int **out) {
    int *a = calloc((size_t)n, sizeof(int));
    if (!a) return 0;
    const char *p = s;
    for (int i = 0; i < n; i++) {
        while (*p == ' ' || *p == '\t') p++;
        char *end;
        errno = 0;
        long v = strtol(p, &end, 10);
        if (errno || end == p || v < 1 || v > INT_MAX || (i && v < a[i - 1])) {
            free(a);
            return 0;
        }
        a[i] = (int)v;
        p = end;
        while (*p == ' ' || *p == '\t') p++;
        if (i + 1 < n) {
            if (*p != ',') { free(a); return 0; }
            p++;
        }
    }
    while (*p == ' ' || *p == '\t') p++;
    if (*p) { free(a); return 0; }
    *out = a;
    return 1;
}

// Конфигурация хранится в простом формате key=value, чтобы тесты можно было повторять.
int config_load(const char *path, Config *cfg, int **initial) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    config_defaults(cfg);
    int *a = NULL;
    char initial_text[4096] = "";
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#' || *p == ';') continue;
        char *eq = strchr(p, '=');
        if (!eq) { fclose(f); free(a); return 0; }
        *eq++ = '\0';
        char *end = p + strlen(p);
        while (end > p && isspace((unsigned char)end[-1])) *--end = '\0';
        while (isspace((unsigned char)*eq)) eq++;
        end = eq + strlen(eq);
        while (end > eq && isspace((unsigned char)end[-1])) *--end = '\0';
        long v;
        if (!strcmp(p, "initial")) {
            if (strlen(eq) >= sizeof(initial_text)) { fclose(f); return 0; }
            snprintf(initial_text, sizeof(initial_text), "%s", eq);
        } else if (!strcmp(p, "strategy")) {
            if (!strcmp(eq, "READ_OLD")) cfg->strategy = READ_OLD;
            else if (!strcmp(eq, "WAIT_NEW")) cfg->strategy = WAIT_NEW;
            else { fclose(f); free(a); return 0; }
        } else {
            if (!parse_long(eq, 0, LONG_MAX, &v)) { fclose(f); free(a); return 0; }
            if ((strcmp(p, "seed") && v > INT_MAX) || (!strcmp(p, "seed") && (unsigned long)v > UINT_MAX)) { fclose(f); free(a); return 0; }
            if (!strcmp(p, "size")) cfg->n = (int)v;
            else if (!strcmp(p, "readers")) cfg->readers = (int)v;
            else if (!strcmp(p, "writers")) cfg->writers = (int)v;
            else if (!strcmp(p, "ops")) cfg->ops = (int)v;
            else if (!strcmp(p, "min")) cfg->min_value = (int)v;
            else if (!strcmp(p, "max")) cfg->max_value = (int)v;
            else if (!strcmp(p, "stages")) cfg->stages = (int)v;
            else if (!strcmp(p, "read_ms")) cfg->read_ms = (int)v;
            else if (!strcmp(p, "prep_ms")) cfg->prep_ms = (int)v;
            else if (!strcmp(p, "confirm_ms")) cfg->confirm_ms = (int)v;
            else if (!strcmp(p, "seed")) cfg->seed = (unsigned int)v;
            else { fclose(f); free(a); return 0; }
        }
    }
    fclose(f);
    if (cfg->n < 1 || cfg->n > IDZ3_MAX_DB || cfg->readers < 1 || cfg->readers > IDZ3_MAX_PARTICIPANTS ||
        cfg->writers < 1 || cfg->writers > IDZ3_MAX_PARTICIPANTS || cfg->ops < 0 || cfg->ops > 1000000 ||
        cfg->min_value < 1 || cfg->max_value < cfg->min_value || cfg->stages < 1 || cfg->stages > 100000 ||
        cfg->read_ms > 60000 || cfg->prep_ms > 60000 || cfg->confirm_ms > 60000) { free(a); return 0; }
    if (initial_text[0] && !parse_initial(initial_text, cfg->n, &a)) return 0;
    if (!a) {
        a = calloc((size_t)cfg->n, sizeof(int));
        if (!a) return 0;
        for (int i = 0; i < cfg->n; i++) a[i] = i + 1;
    }
    *initial = a;
    return 1;
}

static int ask_number(const char *label, long min, long max, int *out) {
    char line[128];
    for (;;) {
        if (read_line(label, line, sizeof(line)) <= 0) return 0;
        long v;
        if (parse_long(line, min, max, &v)) { *out = (int)v; return 1; }
        dprintf(STDOUT_FILENO, "Введите целое число от %ld до %ld.\n", min, max);
    }
}

// Интерактивный ввод использует файловый дескриптор stdin и проверяет каждое значение.
int config_interactive(Config *c, int **initial) {
    config_defaults(c);
    if (!ask_number("Размер базы (1..100000): ", 1, IDZ3_MAX_DB, &c->n)) return 0;
    int *a = calloc((size_t)c->n, sizeof(int));
    if (!a) return 0;
    dprintf(STDOUT_FILENO, "Введите %d положительных чисел по неубыванию.\n", c->n);
    for (int i = 0; i < c->n; i++) {
        char prompt[64];
        snprintf(prompt, sizeof(prompt), "a[%d]: ", i);
        int low = i ? a[i - 1] : 1;
        if (!ask_number(prompt, low, INT_MAX, &a[i])) { free(a); return 0; }
    }
    if (!ask_number("Количество читателей: ", 1, IDZ3_MAX_PARTICIPANTS, &c->readers) ||
        !ask_number("Количество писателей: ", 1, IDZ3_MAX_PARTICIPANTS, &c->writers) ||
        !ask_number("Операций на участника (0 = без ограничения): ", 0, 1000000, &c->ops) ||
        !ask_number("Минимальное новое значение: ", 1, INT_MAX, &c->min_value) ||
        !ask_number("Максимальное новое значение: ", c->min_value, INT_MAX, &c->max_value) ||
        !ask_number("Количество стадий подготовки: ", 1, 100000, &c->stages)) { free(a); return 0; }
    char line[64];
    for (;;) {
        if (read_line("Стратегия читателей (READ_OLD / WAIT_NEW): ", line, sizeof(line)) <= 0) { free(a); return 0; }
        if (!strcmp(line, "READ_OLD")) { c->strategy = READ_OLD; break; }
        if (!strcmp(line, "WAIT_NEW")) { c->strategy = WAIT_NEW; break; }
        dprintf(STDOUT_FILENO, "Введите READ_OLD или WAIT_NEW.\n");
    }
    if (!ask_number("Задержка чтения, мс: ", 0, 60000, &c->read_ms) ||
        !ask_number("Задержка стадии подготовки, мс: ", 0, 60000, &c->prep_ms) ||
        !ask_number("Задержка подтверждения, мс: ", 0, 60000, &c->confirm_ms)) { free(a); return 0; }
    c->seed = 20261110u;
    *initial = a;
    return 1;
}

void config_usage(const char *p) {
    dprintf(STDOUT_FILENO,
        "Usage: %s [--config FILE] [--seed N] [--ops N] [--strategy READ_OLD|WAIT_NEW]\n"
        "       [--readers N] [--writers N] [--read-ms N] [--prep-ms N] [--confirm-ms N]\n"
        "Without arguments the program asks for settings interactively.\n", p);
}

int config_apply_args(int argc, char **argv, Config *cfg, int **initial, const char **config_path) {
    *config_path = NULL;
    int loaded = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help")) { config_usage(argv[0]); return -1; }
        if (!strcmp(argv[i], "--config") && i + 1 < argc) { *config_path = argv[++i]; }
    }
    if (*config_path) {
        if (!config_load(*config_path, cfg, initial)) return 0;
        loaded = 1;
    } else {
        config_defaults(cfg);
        *initial = NULL;
    }
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config")) { i++; continue; }
        if (i + 1 >= argc) return 0;
        const char *key = argv[i++], *val = argv[i];
        long v;
        if (!strcmp(key, "--strategy")) {
            if (!strcmp(val, "READ_OLD")) cfg->strategy = READ_OLD;
            else if (!strcmp(val, "WAIT_NEW")) cfg->strategy = WAIT_NEW;
            else return 0;
        } else {
            if (!parse_long(val, 0, LONG_MAX, &v)) return 0;
            if ((strcmp(key, "--seed") && v > INT_MAX) || (!strcmp(key, "--seed") && (unsigned long)v > UINT_MAX)) return 0;
            if (!strcmp(key, "--seed")) cfg->seed = (unsigned int)v;
            else if (!strcmp(key, "--ops")) cfg->ops = (int)v;
            else if (!strcmp(key, "--readers")) cfg->readers = (int)v;
            else if (!strcmp(key, "--writers")) cfg->writers = (int)v;
            else if (!strcmp(key, "--read-ms")) cfg->read_ms = (int)v;
            else if (!strcmp(key, "--prep-ms")) cfg->prep_ms = (int)v;
            else if (!strcmp(key, "--confirm-ms")) cfg->confirm_ms = (int)v;
            else return 0;
        }
        i++;
    }
    if (cfg->n < 1 || cfg->n > IDZ3_MAX_DB || cfg->readers < 1 || cfg->readers > IDZ3_MAX_PARTICIPANTS ||
        cfg->writers < 1 || cfg->writers > IDZ3_MAX_PARTICIPANTS || cfg->ops < 0 || cfg->ops > 1000000 ||
        cfg->min_value < 1 || cfg->max_value < cfg->min_value || cfg->stages < 1 || cfg->stages > 100000 ||
        cfg->read_ms > 60000 || cfg->prep_ms > 60000 || cfg->confirm_ms > 60000) return 0;
    if (!loaded) {
        *initial = calloc((size_t)cfg->n, sizeof(int));
        if (!*initial) return 0;
        for (int i = 0; i < cfg->n; i++) (*initial)[i] = i + 1;
    }
    return 1;
}
