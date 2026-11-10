#define _POSIX_C_SOURCE 200809L
#include "../common/config.h"
#include "../common/model.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef USE_UNNAMED_SEMAPHORES
#define USE_UNNAMED_SEMAPHORES 0
#endif

typedef struct { pid_t pid; WorkerRole role; int id; int done; } Child;

// Родитель создаёт POSIX shared memory, семафоры и по одному процессу на участника.
static int run_parent(Config *cfg, const int *initial) {
    char shm_name[IDZ3_NAME_LEN];
    snprintf(shm_name, sizeof(shm_name), "/akos3_parent_%s_%ld", USE_UNNAMED_SEMAPHORES ? "unnamed" : "named", (long)getpid());
    int fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd == -1) { perror("shm_open"); return 0; }
    size_t size = shared_size(cfg->n);
    if (ftruncate(fd, (off_t)size) == -1) { perror("ftruncate"); close(fd); shm_unlink(shm_name); return 0; }
    Shared *sh = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (sh == MAP_FAILED) { perror("mmap"); close(fd); shm_unlink(shm_name); return 0; }
    memset(sh, 0, size);
    sh->cfg = *cfg;
    sh->version = 1;
    snprintf(sh->shm_name, sizeof(sh->shm_name), "%s", shm_name);
    mkdir("logs", 0755);
    snprintf(sh->log_path, sizeof(sh->log_path), "logs/idz3_parent_%s_%ld.log",
             USE_UNNAMED_SEMAPHORES ? "unnamed" : "named", (long)getpid());
    memcpy(shared_buffer(sh, 0), initial, (size_t)cfg->n * sizeof(int));
    memcpy(shared_buffer(sh, 1), initial, (size_t)cfg->n * sizeof(int));

    SemSet sems;
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "akos3_%s_%ld", USE_UNNAMED_SEMAPHORES ? "u" : "n", (long)getpid());
    if (!semset_init(sh, &sems, !USE_UNNAMED_SEMAPHORES, prefix)) {
        perror("semaphore initialization"); munmap(sh, size); close(fd); shm_unlink(shm_name); return 0;
    }
    install_signal_handlers();
    log_event(sh, &sems, "============================================================");
    log_event(sh, &sems, "Readers and writers: process simulation started");
    log_event(sh, &sems, "IPC: POSIX shared memory; semaphores: %s", USE_UNNAMED_SEMAPHORES ? "unnamed, process-shared" : "named POSIX");
    log_event(sh, &sems, "Database version=1; size=%d; readers=%d; writers=%d; operations=%s; strategy=%s; seed=%u",
              cfg->n, cfg->readers, cfg->writers, cfg->ops ? "finite" : "unlimited",
              cfg->strategy == READ_OLD ? "READ_OLD" : "WAIT_NEW", cfg->seed);
    log_event(sh, &sems, "Initial database: ");
    print_database(initial, cfg->n);
    (void)write(STDOUT_FILENO, "\n", 1);

    int total = cfg->readers + cfg->writers;
    Child *children = calloc((size_t)total, sizeof(*children));
    if (!children) {
        request_stop(sh, &sems); semset_destroy(sh, &sems); munmap(sh, size); close(fd); shm_unlink(shm_name); return 0;
    }
    int launched = 0;
    for (int role = ROLE_WRITER; role >= ROLE_READER; role--) {
        int count = role == ROLE_WRITER ? cfg->writers : cfg->readers;
        for (int i = 0; i < count; i++) {
            pid_t pid = fork();
            if (pid == -1) {
                perror("fork");
                request_stop(sh, &sems);
                goto wait_children;
            }
            if (pid == 0) {
                SemSet child_sems;
                if (!semset_attach(sh, &child_sems, !USE_UNNAMED_SEMAPHORES)) _exit(2);
                int ok = run_worker(sh, &child_sems, (WorkerRole)role, i + 1, -1);
                semset_close(&child_sems);
                _exit(ok ? 0 : 1);
            }
            children[launched++] = (Child){.pid = pid, .role = (WorkerRole)role, .id = i + 1, .done = 0};
        }
    }

// Родитель собирает статусы всех потомков и при ошибке останавливает оставшиеся процессы.
wait_children: ;
    int remaining = launched;
    int failed = launched != total;
    while (remaining > 0) {
        if (interrupt_requested()) {
            log_event(sh, &sems, "Stop requested from keyboard (SIGINT)");
            request_stop(sh, &sems);
            for (int i = 0; i < launched; i++) if (!children[i].done) kill(children[i].pid, SIGTERM);
        }
        for (int i = 0; i < launched; i++) {
            if (children[i].done) continue;
            int status;
            pid_t got = waitpid(children[i].pid, &status, WNOHANG);
            if (got == children[i].pid) {
                children[i].done = 1;
                remaining--;
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                    failed = 1;
                    request_stop(sh, &sems);
                    for (int j = 0; j < launched; j++) if (!children[j].done) kill(children[j].pid, SIGTERM);
                }
            }
        }
        if (remaining) {
            struct timespec pause = {0, 30000000L};
            nanosleep(&pause, NULL);
        }
    }
    while (sem_wait(sems.state) == -1 && errno == EINTR) {}
    {
        sh->failed |= failed;
        log_event(sh, &sems, "============================================================");
        log_event(sh, &sems, "%s", sh->failed ? "Simulation finished with an error" : interrupt_requested() ? "Simulation stopped by user" : "Simulation completed");
        log_event(sh, &sems, "Final version=%d; reads=%lld; publications=%lld; stages=%lld; aborted writes=%lld",
                  sh->version, sh->reads, sh->publications, sh->stages_done, sh->aborted_writes);
        log_event(sh, &sems, "Final database:");
        print_database(shared_buffer(sh, sh->active_buffer), cfg->n);
        (void)write(STDOUT_FILENO, "\n", 1);
        sem_post(sems.state);
    }
    log_event(sh, &sems, "Log saved to %s", sh->log_path);
    free(children);
    semset_destroy(sh, &sems);
    munmap(sh, size);
    close(fd);
    shm_unlink(shm_name);
    return !failed;
}

int main(int argc, char **argv) {
    Config cfg;
    int *initial = NULL;
    const char *config_path = NULL;
    int rc;
    if (argc == 1) rc = config_interactive(&cfg, &initial);
    else rc = config_apply_args(argc, argv, &cfg, &initial, &config_path);
    if (rc == -1) return EXIT_SUCCESS;
    if (!rc) { dprintf(STDERR_FILENO, "Invalid configuration. Use --help or check the config file.\n"); return EXIT_FAILURE; }
    if (!validate_database(initial, cfg.n)) {
        dprintf(STDERR_FILENO, "Initial database must contain positive values in nondecreasing order.\n"); free(initial); return EXIT_FAILURE;
    }
    int ok = run_parent(&cfg, initial);
    free(initial);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
