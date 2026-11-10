#define _POSIX_C_SOURCE 200809L
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <semaphore.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    (void)argc;
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "mq_stop")) {
        mqd_t queue = mq_open(MQ_REQUEST_NAME, O_WRONLY);
        if (queue == (mqd_t)-1) { perror("mq_open: сервер уже остановлен?"); return 1; }
        Message m = {.type = MSG_STOP, .pid = (int)getpid()};
        int ok = mq_send(queue, (const char *)&m, sizeof(m), 0) == 0;
        if (!ok) perror("mq_send");
        mq_close(queue);
        return ok ? 0 : 1;
    }
    if (!strcmp(name, "mq_cleanup")) {
        mq_unlink(MQ_REQUEST_NAME);
        int a = sem_unlink(MQ_STATE_SEM);
        int b = sem_unlink(MQ_WRITER_SEM);
        int c = sem_unlink(MQ_LOG_SEM);
        if (a == -1 && errno != ENOENT) perror("sem_unlink state");
        if (b == -1 && errno != ENOENT) perror("sem_unlink writer");
        if (c == -1 && errno != ENOENT) perror("sem_unlink log");
        dprintf(STDOUT_FILENO, "POSIX message queue and named semaphores removed.\n");
        return 0;
    }
    fprintf(stderr, "Unknown command: %s\n", name);
    return 1;
}
