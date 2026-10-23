#ifndef LOG_ASYNC_LOG_H
#define LOG_ASYNC_LOG_H

#include <stddef.h>
#include <stdio.h>

#define ASYNC_LOG_RING_CAPACITY 4096

#include "log/log.h"

typedef struct async_log async_log;

/**
 * Start a background thread that writes log lines to out, which must stay open until
 * async_log_close. Each thread that logs gets its own ring of ASYNC_LOG_RING_CAPACITY lines at
 * its first call, for at most rings threads; lines of further threads are dropped.
 * A full ring drops the line; the writer then logs "dropped N lines".
 * Return NULL on failure.
 */
async_log *async_log_new(FILE *out, size_t rings);

/**
 * Return a logger that writes to l. Its log call never blocks or does IO; it truncates lines
 * to 231 bytes. Lines of one thread keep their order; lines of different threads may not.
 */
logger async_log_logger(async_log *l);

/**
 * Write all queued lines, flush out, stop the writer thread, and free l. Call after all threads
 * stop logging to l; out stays open.
 */
void async_log_close(async_log *l);

#endif
