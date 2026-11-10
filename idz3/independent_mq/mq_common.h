#ifndef IDZ3_MQ_COMMON_H
#define IDZ3_MQ_COMMON_H
#include "protocol.h"
#include "../common/config.h"

void mq_install_signals(void);
void mq_set_server_pid(pid_t pid);
int mq_was_interrupted(void);
int mq_wait_semaphore(sem_t *sem);
int mq_send_force(mqd_t queue, const Message *message);
int mq_send_wait(mqd_t queue, const Message *message);
int mq_receive_wait(mqd_t queue, Message *message);
int mq_create_reply(char *name, size_t size, mqd_t *queue);
void mq_close_reply(const char *name, mqd_t queue);
int mq_client_open(mqd_t *server, char *reply_name, size_t reply_size, mqd_t *reply);
int mq_request(mqd_t server, mqd_t reply, const Message *request, Message *response);
int mq_sleep(int ms);
void mq_log(sem_t *log_sem, const char *path, const char *fmt, ...);
int mq_open_named_sems(sem_t **state, sem_t **writer, sem_t **log_sem, int create);
void mq_close_named_sems(sem_t *state, sem_t *writer, sem_t *log_sem);

#endif
