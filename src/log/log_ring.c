#include "log/log_ring.h"

#include <stdlib.h>

/** Allocate capacity records; head and tail count records ever pushed and popped. */
int log_ring_init(log_ring *r, size_t capacity) {
  if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
    return -1;
  }
  r->records = malloc(capacity * sizeof(*r->records));
  if (r->records == NULL) {
    return -1;
  }
  atomic_init(&r->head, 0);
  atomic_init(&r->tail, 0);
  atomic_init(&r->dropped, 0);
  r->mask = capacity - 1;
  return 0;
}

/** Free the record array of r. */
void log_ring_free(log_ring *r) {
  free(r->records);
  r->records = NULL;
}

/** Return the record at head, or NULL and count a drop when the consumer is mask + 1 behind. */
log_record *log_ring_reserve(log_ring *r) {
  size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
  if (head - tail > r->mask) {
    atomic_fetch_add_explicit(&r->dropped, 1, memory_order_relaxed);
    return NULL;
  }
  return &r->records[head & r->mask];
}

/** Advance head with release order, so the consumer sees the filled record. */
void log_ring_commit(log_ring *r) {
  size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
  atomic_store_explicit(&r->head, head + 1, memory_order_release);
}

/** Return the record at tail, or NULL when tail has reached head. */
const log_record *log_ring_peek(log_ring *r) {
  size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
  if (tail == head) {
    return NULL;
  }
  return &r->records[tail & r->mask];
}

/** Advance tail with release order, so the producer reuses the slot only after it is read. */
void log_ring_release(log_ring *r) {
  size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
  atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
}

/** Atomically read and reset the drop count of r. */
size_t log_ring_take_dropped(log_ring *r) {
  return atomic_exchange_explicit(&r->dropped, 0, memory_order_relaxed);
}
