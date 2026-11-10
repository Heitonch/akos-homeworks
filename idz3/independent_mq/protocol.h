#ifndef IDZ3_MQ_PROTOCOL_H
#define IDZ3_MQ_PROTOCOL_H
#include <mqueue.h>
#include <sys/types.h>

#define MQ_REQUEST_NAME "/akos3_mq_requests"
#define MQ_STATE_SEM "/akos3_mq_state"
#define MQ_WRITER_SEM "/akos3_mq_writer"
#define MQ_LOG_SEM "/akos3_mq_log"
#define MQ_REPLY_LEN 64
#define MQ_PENDING_MAX 2048

// Все сообщения имеют фиксированный размер; ответ возвращается в отдельную очередь клиента.
typedef enum {
    MSG_READ = 1,
    MSG_HELLO,
    MSG_WRITE_BEGIN,
    MSG_WRITE_STAGE,
    MSG_WRITE_PUBLISH,
    MSG_WRITE_ABORT,
    MSG_STOP,
    MSG_CANCEL,
    MSG_RESPONSE
} MessageType;

typedef struct {
    int type;                 // тип запроса или ответа
    int pid;                  // PID клиента, отправившего сообщение
    int server_pid;           // PID сервера, передаваемый при подключении
    int id;                   // пользовательский номер участника
    int operation;            // номер операции клиента
    int stage;                // номер стадии подготовки
    int index;                // индекс записи для чтения
    unsigned int seed;        // seed клиента или параметры конфигурации сервера
    int version;              // номер версии в ответе
    int value;                // прочитанное значение
    int status;               // 0 — успешно, 1 — запрос отклонён
    int n;                    // размер базы из конфигурации сервера
    int min_value;             // границы генерации новых значений
    int max_value;
    int stages;               // число стадий подготовки
    int strategy;             // READ_OLD или WAIT_NEW
    int read_ms;              // задержка чтения
    int prep_ms;              // задержка подготовки
    int confirm_ms;           // задержка подтверждения
    int ops;                  // операций по умолчанию
    long long product;        // value * (index + 1)
    char reply[MQ_REPLY_LEN]; // имя очереди ответов клиента
} Message;

#endif
