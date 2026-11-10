CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -D_POSIX_C_SOURCE=200809L
LDLIBS = -lrt -pthread

COMMON = common/config.c common/shared_sim.c
MQ_COMMON = independent_mq/mq_common.c
TOOLS = parent_named parent_unnamed shm_init shm_reader shm_writer shm_stop shm_status shm_cleanup mq_server mq_reader mq_writer mq_stop mq_cleanup
BINS = $(addprefix bin/,$(TOOLS))

.PHONY: all clean test
all: $(BINS)

bin:
	mkdir -p bin

bin/parent_named: parent_named/main.c $(COMMON) common/model.h common/config.h | bin
	$(CC) $(CFLAGS) -o $@ parent_named/main.c $(COMMON) $(LDLIBS)

bin/parent_unnamed: parent_unnamed/main.c $(COMMON) common/model.h common/config.h | bin
	$(CC) $(CFLAGS) -DUSE_UNNAMED_SEMAPHORES=1 -o $@ parent_unnamed/main.c $(COMMON) $(LDLIBS)

$(addprefix bin/,shm_init shm_reader shm_writer shm_stop shm_status shm_cleanup): bin/%: independent_shm/tool.c $(COMMON) common/model.h common/config.h | bin
	$(CC) $(CFLAGS) -o $@ independent_shm/tool.c $(COMMON) $(LDLIBS)

bin/mq_server: independent_mq/server.c $(MQ_COMMON) $(COMMON) | bin
	$(CC) $(CFLAGS) -o $@ independent_mq/server.c $(MQ_COMMON) $(COMMON) $(LDLIBS)

bin/mq_reader: independent_mq/reader.c $(MQ_COMMON) common/config.c common/config.h common/model.h | bin
	$(CC) $(CFLAGS) -o $@ independent_mq/reader.c $(MQ_COMMON) common/config.c $(LDLIBS)

bin/mq_writer: independent_mq/writer.c $(MQ_COMMON) common/config.c common/config.h common/model.h | bin
	$(CC) $(CFLAGS) -o $@ independent_mq/writer.c $(MQ_COMMON) common/config.c $(LDLIBS)

$(addprefix bin/,mq_stop mq_cleanup): bin/%: independent_mq/control.c independent_mq/protocol.h | bin
	$(CC) $(CFLAGS) -o $@ independent_mq/control.c $(LDLIBS)

test: all
	python3 tests/check.py

clean:
	rm -rf bin build
	rm -f logs/*.log
