#ifndef IDZ3_MODEL_H
#define IDZ3_MODEL_H

#define _POSIX_C_SOURCE 200809L
#include <semaphore.h>
#include <stddef.h>
#include <sys/types.h>

#define IDZ3_MAX_DB 100000
#define IDZ3_MAX_PARTICIPANTS 256
#define IDZ3_NAME_LEN 64
#define IDZ3_PATH_LEN 256
#define IDZ3_LOG_LINE 1200

typedef enum { READ_OLD = 0, WAIT_NEW = 1 } Strategy;
typedef enum { ROLE_READER = 1, ROLE_WRITER = 2 } WorkerRole;

// Общие параметры моделирования; одна структура используется во всех четырёх вариантах.
typedef struct {
    int n;                 // количество записей в базе
    int readers;           // ожидаемое число читателей
    int writers;           // ожидаемое число писателей
    int ops;               // операций на участника; 0 означает бесконечный режим
    int min_value;          // нижняя граница новых значений
    int max_value;          // верхняя граница новых значений
    int stages;             // число этапов подготовки версии
    Strategy strategy;      // режим поведения читателей
    int read_ms;             // задержка после чтения
    int prep_ms;             // задержка между стадиями
    int confirm_ms;          // задержка перед публикацией
    unsigned int seed;       // начальное состояние генератора случайных чисел
} Config;

// Область POSIX shared memory. Массивы двух буферов расположены в конце структуры.
// Все поля состояния и статистики меняются под защитой state-семафора.
typedef struct {
    int version;                    // номер текущей подтверждённой версии
    int active_buffer;              // индекс буфера, доступного читателям
    int writing;                    // признак незавершённой подготовки
    int stop;                       // общий запрос остановки участников
    int failed;                     // признак внутренней ошибки
    int waiting_readers;            // число читателей, ожидающих публикации
    int active_workers;             // число подключённых независимых участников
    long long reads;                // количество завершённых чтений
    long long publications;         // количество опубликованных версий
    long long stages_done;          // количество завершённых стадий
    long long aborted_writes;       // количество отменённых подготовок
    Config cfg;                     // параметры текущего сеанса
    char shm_name[IDZ3_NAME_LEN];   // имя объекта разделяемой памяти
    char sem_names[4][IDZ3_NAME_LEN]; // имена семафоров для именованного режима
    char log_path[IDZ3_PATH_LEN];   // путь к общему журналу
    sem_t local_sems[4];            // семафоры для варианта с неименованной синхронизацией
    int data[];                     // два массива по cfg.n элементов
} Shared;

// Локальные адреса семафоров: указатели на именованные либо на встроенные неименованные семафоры.
typedef struct {
    sem_t *state;       // защита метаданных и переключения активного буфера
    sem_t *writer;      // взаимное исключение между писателями
    sem_t *changed;     // пробуждение читателей после публикации или отмены
    sem_t *log;         // последовательная запись в общий журнал
    int named;          // 1 — sem_open, 0 — семафоры внутри Shared
} SemSet;

size_t shared_size(int n);
int *shared_buffer(Shared *sh, int index);
int semset_init(Shared *sh, SemSet *ss, int named, const char *prefix);
int semset_attach(Shared *sh, SemSet *ss, int named);
void semset_close(SemSet *ss);
void semset_destroy(Shared *sh, SemSet *ss);
int sem_wait_safe(sem_t *sem);
int sem_post_many(sem_t *sem, int count);
void log_event(Shared *sh, SemSet *ss, const char *fmt, ...);
void request_stop(Shared *sh, SemSet *ss);
int is_stopped(Shared *sh, SemSet *ss);
int delay_ms(Shared *sh, SemSet *ss, int ms);
unsigned int random_next(unsigned int *seed);
int random_between(unsigned int *seed, int low, int high);
int validate_database(const int *data, int n);
void print_database(const int *data, int n);
void install_signal_handlers(void);
int interrupt_requested(void);
int run_worker(Shared *sh, SemSet *ss, WorkerRole role, int id, int ops_override);

#endif
