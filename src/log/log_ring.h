#ifndef LOG_LOG_RING_H
#define LOG_LOG_RING_H

#include <stdatomic.h>
#include <stddef.h>
#include <time.h>

#include "log/log.h"

typedef struct {
  struct timespec when;
  log_level level;
  unsigned len;
  // Not NUL-terminated; holds len bytes.
  char text[232];
} log_record;

/** A bounded single-producer, single-consumer queue of log records. */
typedef struct {
  // Separate cache lines: the producer writes head, the consumer writes tail.
  _Alignas(64) atomic_size_t head;
  _Alignas(64) atomic_size_t tail;
  atomic_size_t dropped;
  size_t mask;
  log_record *records;
} log_ring;

/** Initialize an empty ring of capacity records, a power of two; return 0 or -1. */
int log_ring_init(log_ring *r, size_t capacity);

/** Free the records of r. */
void log_ring_free(log_ring *r);

/**
 * Producer: return the next free record to fill, or NULL and count one drop if r is full.
 * The record reaches the consumer at log_ring_commit.
 */
log_record *log_ring_reserve(log_ring *r);

/** Producer: publish the record of the last log_ring_reserve. */
void log_ring_commit(log_ring *r);

/** Consumer: return the oldest committed record, or NULL if r is empty. */
const log_record *log_ring_peek(log_ring *r);

/** Consumer: release the record of the last log_ring_peek for reuse. */
void log_ring_release(log_ring *r);

/** Return the drop count of r and reset it to 0; any thread. */
size_t log_ring_take_dropped(log_ring *r);

#endif
