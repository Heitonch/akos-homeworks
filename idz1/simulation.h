#ifndef SIMULATION_H
#define SIMULATION_H

typedef enum { READ_OLD, WAIT_NEW } Strategy;

typedef struct {
    int n;
    int *a;
    int ver;
} Db;

typedef struct {
    int id;
    int left;
    long long done;
} Reader;

typedef struct {
    int id;
    int left;
    int active;
    int stage;
    int *buf;
    long long done;
} Writer;

typedef struct {
    int readers;
    int writers;
    int ops;
    int min;
    int max;
    int stages;
    Strategy strategy;
    int read_ms;
    int prep_ms;
    int confirm_ms;
} Config;

int run(Db *db, const Config *c);

#endif