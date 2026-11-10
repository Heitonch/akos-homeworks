#define _POSIX_C_SOURCE 200809L
#include "../common/config.h"
#include "../common/model.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SESSION_SHM "/akos3_independent_shm"
#define SESSION_PREFIX "akos3_independent"

typedef struct { int fd; size_t size; Shared *sh; SemSet sems; } Session;

// Каждый исполняемый файл самостоятельно подключается к уже созданному сеансу.
static int attach_session(Session *s) {
    memset(s, 0, sizeof(*s));
    s->fd = shm_open(SESSION_SHM, O_RDWR, 0600);
    if (s->fd == -1) { perror("shm_open: сначала запустите shm_init"); return 0; }
    struct stat st;
    if (fstat(s->fd, &st) == -1 || st.st_size < (off_t)sizeof(Shared)) { perror("fstat"); close(s->fd); return 0; }
    s->size = (size_t)st.st_size;
    s->sh = mmap(NULL, s->size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->sh == MAP_FAILED) { perror("mmap"); close(s->fd); return 0; }
    if (!semset_attach(s->sh, &s->sems, 1)) {
        fprintf(stderr, "Не удалось открыть семафоры сеанса.\n");
        munmap(s->sh, s->size); close(s->fd); return 0;
    }
    return 1;
}

static void detach_session(Session *s) {
    semset_close(&s->sems);
    if (s->sh && s->sh != MAP_FAILED) munmap(s->sh, s->size);
    if (s->fd >= 0) close(s->fd);
}

// Инициализатор создаёт ресурсы, но не запускает читателей и писателей.
static int init_session(int argc, char **argv) {
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
    int fd = shm_open(SESSION_SHM, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) {
        if (errno == EEXIST) fprintf(stderr, "Сеанс уже существует. Сначала выполните shm_cleanup.\n");
        else perror("shm_open");
        free(initial); return 1;
    }
    size_t size = shared_size(cfg.n);
    if (ftruncate(fd, (off_t)size) == -1) { perror("ftruncate"); close(fd); shm_unlink(SESSION_SHM); free(initial); return 1; }
    Shared *sh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("mmap"); close(fd); shm_unlink(SESSION_SHM); free(initial); return 1; }
    memset(sh, 0, size);
    sh->cfg = cfg;
    sh->version = 1;
    snprintf(sh->shm_name, sizeof(sh->shm_name), "%s", SESSION_SHM);
    mkdir("logs", 0755);
    snprintf(sh->log_path, sizeof(sh->log_path), "logs/idz3_independent_shm.log");
    memcpy(shared_buffer(sh, 0), initial, (size_t)cfg.n * sizeof(int));
    memcpy(shared_buffer(sh, 1), initial, (size_t)cfg.n * sizeof(int));
    SemSet sems;
    if (!semset_init(sh, &sems, 1, SESSION_PREFIX)) {
        perror("sem_open"); munmap(sh, size); close(fd); shm_unlink(SESSION_SHM); free(initial); return 1;
    }
    log_event(sh, &sems, "[SHM INIT] Независимый сеанс создан");
    log_event(sh, &sems, "База: %d элементов; читателей=%d; писателей=%d; операций=%s; стратегия=%s; seed=%u",
              cfg.n, cfg.readers, cfg.writers, cfg.ops ? "конечное число" : "без ограничения",
              cfg.strategy == READ_OLD ? "READ_OLD" : "WAIT_NEW", cfg.seed);
    log_event(sh, &sems, "Начальная версия: 1");
    log_event(sh, &sems, "Журнал событий: %s", sh->log_path);
    log_event(sh, &sems, "Для запуска используйте отдельные процессы shm_reader и shm_writer; остановка: shm_stop; очистка: shm_cleanup");
    semset_close(&sems);
    munmap(sh, size);
    close(fd);
    free(initial);
    return 0;
}

static int worker_tool(int argc, char **argv, WorkerRole role) {
    int id = 1, ops = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--id") && i + 1 < argc) id = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ops") && i + 1 < argc) ops = atoi(argv[++i]);
        else { fprintf(stderr, "Использование: %s [--id N] [--ops N]\n", argv[0]); return 1; }
    }
    if (id < 1 || id > IDZ3_MAX_PARTICIPANTS || ops < -1 || ops > 1000000) return 1;
    Session s;
    if (!attach_session(&s)) return 1;
    int ok = run_worker(s.sh, &s.sems, role, id, ops);
    detach_session(&s);
    return ok ? 0 : 1;
}

static int stop_tool(void) {
    Session s;
    if (!attach_session(&s)) return 1;
    request_stop(s.sh, &s.sems);
    log_event(s.sh, &s.sems, "[SHM STOP] Остановка запрошена");
    detach_session(&s);
    return 0;
}

static int status_tool(void) {
    Session s;
    if (!attach_session(&s)) return 1;
    if (sem_wait_safe(s.sems.state) == 0) {
        dprintf(STDOUT_FILENO, "version=%d writing=%d stop=%d active_workers=%d reads=%lld publications=%lld stages=%lld aborted=%lld\n",
                s.sh->version, s.sh->writing, s.sh->stop, s.sh->active_workers,
                s.sh->reads, s.sh->publications, s.sh->stages_done, s.sh->aborted_writes);
        sem_post(s.sems.state);
    }
    detach_session(&s);
    return 0;
}

// Очистка сначала просит участников остановиться и только затем удаляет IPC-объекты.
static int cleanup_tool(int argc, char **argv) {
    int probe = shm_open(SESSION_SHM, O_RDWR, 0600);
    if (probe == -1 && errno == ENOENT) return 0;
    if (probe >= 0) close(probe);
    int force = argc > 1 && !strcmp(argv[1], "--force");
    Session s;
    if (!attach_session(&s)) {
        if (errno == ENOENT) return 0;
        return 1;
    }
    request_stop(s.sh, &s.sems);
    for (int i = 0; i < 100; i++) {
        if (sem_wait_safe(s.sems.state) == -1) break;
        int active = s.sh->active_workers;
        sem_post(s.sems.state);
        if (active == 0) break;
        struct timespec ts = {0, 50000000L};
        nanosleep(&ts, NULL);
    }
    if (sem_wait_safe(s.sems.state) == 0) {
        int active = s.sh->active_workers;
        sem_post(s.sems.state);
        if (active && !force) {
            fprintf(stderr, "Ещё работают %d процесс(а). Повторите позже или используйте --force.\n", active);
            detach_session(&s);
            return 1;
        }
    }
    log_event(s.sh, &s.sems, "[SHM CLEANUP] Итог: version=%d reads=%lld publications=%lld stages=%lld aborted=%lld",
              s.sh->version, s.sh->reads, s.sh->publications, s.sh->stages_done, s.sh->aborted_writes);
    log_event(s.sh, &s.sems, "[SHM CLEANUP] Удаление семафоров и разделяемой памяти");
    semset_destroy(s.sh, &s.sems);
    munmap(s.sh, s.size);
    close(s.fd);
    shm_unlink(SESSION_SHM);
    return 0;
}

int main(int argc, char **argv) {
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "shm_init")) return init_session(argc, argv);
    if (!strcmp(name, "shm_reader")) return worker_tool(argc, argv, ROLE_READER);
    if (!strcmp(name, "shm_writer")) return worker_tool(argc, argv, ROLE_WRITER);
    if (!strcmp(name, "shm_stop")) return stop_tool();
    if (!strcmp(name, "shm_status")) return status_tool();
    if (!strcmp(name, "shm_cleanup")) return cleanup_tool(argc, argv);
    fprintf(stderr, "Неизвестная команда: %s\n", name);
    return 1;
}
