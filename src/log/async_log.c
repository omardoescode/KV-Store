#include "log/async_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "log/log_ring.h"

struct async_log {
  FILE *out;
  log_ring *rings;
  size_t ring_count;
  atomic_size_t claimed;
  // Lines of threads that found no free ring.
  atomic_size_t dropped;
  atomic_int stop;
  // Distinguishes a new logger from a freed one at the same address in thread caches.
  unsigned long id;
  pthread_t writer;
};

static atomic_ulong next_id = 1;

static _Thread_local struct {
  unsigned long id;
  log_ring *ring;
} thread_ring;

static const char *const level_names[] = {
    [LOG_DEBUG] = "DEBUG",
    [LOG_INFO] = "INFO",
    [LOG_WARN] = "WARN",
    [LOG_ERROR] = "ERROR",
};

/** Queue one formatted line in the ring of this thread, claiming one at its first call. */
static void async_log_write(void *ctx, log_level level, const char *fmt, ...) {
  async_log *l = ctx;
  if (thread_ring.id != l->id) {
    size_t i = atomic_fetch_add_explicit(&l->claimed, 1, memory_order_relaxed);
    thread_ring.id = l->id;
    thread_ring.ring = i < l->ring_count ? &l->rings[i] : NULL;
  }
  if (thread_ring.ring == NULL) {
    atomic_fetch_add_explicit(&l->dropped, 1, memory_order_relaxed);
    return;
  }
  log_record *rec = log_ring_reserve(thread_ring.ring);
  if (rec == NULL) {
    return;
  }
  // The writer formats the time: localtime_r takes a global lock in glibc.
  (void)clock_gettime(CLOCK_REALTIME, &rec->when);
  rec->level = level;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(rec->text, sizeof(rec->text), fmt, ap);
  va_end(ap);
  if (n < 0) {
    n = 0;
  }
  rec->len = (size_t)n < sizeof(rec->text) ? (unsigned)n : (unsigned)sizeof(rec->text) - 1;
  log_ring_commit(thread_ring.ring);
}

typedef struct {
  time_t sec;
  char text[32];
} second_cache;

/** Write one line for rec, formatting its second only when it differs from the cached one. */
static void write_line(FILE *out, second_cache *cache, struct timespec when, log_level level,
                       const char *text, int len) {
  if (when.tv_sec != cache->sec) {
    struct tm local;
    if (localtime_r(&when.tv_sec, &local) == NULL ||
        strftime(cache->text, sizeof(cache->text), "%Y-%m-%dT%H:%M:%S", &local) == 0) {
      cache->text[0] = '\0';
    }
    cache->sec = when.tv_sec;
  }
  (void)fprintf(out, "%s.%03ld %s %.*s\n", cache->text, when.tv_nsec / 1000000L, level_names[level],
                len, text);
}

/** Write all committed records and the drop count; return the number of lines written. */
static size_t drain(async_log *l, second_cache *cache) {
  size_t written = 0;
  size_t dropped = atomic_exchange_explicit(&l->dropped, 0, memory_order_relaxed);
  for (size_t i = 0; i != l->ring_count; i++) {
    const log_record *rec;
    while ((rec = log_ring_peek(&l->rings[i])) != NULL) {
      write_line(l->out, cache, rec->when, rec->level, rec->text, (int)rec->len);
      log_ring_release(&l->rings[i]);
      written++;
    }
    dropped += log_ring_take_dropped(&l->rings[i]);
  }
  if (dropped != 0) {
    char text[64];
    int len = snprintf(text, sizeof(text), "dropped %zu lines", dropped);
    struct timespec now;
    (void)clock_gettime(CLOCK_REALTIME, &now);
    write_line(l->out, cache, now, LOG_WARN, text, len);
    written++;
  }
  return written;
}

/** Drain the rings until stopped, flushing out and sleeping whenever they are empty. */
static void *writer_main(void *arg) {
  async_log *l = arg;
  pthread_setname_np(pthread_self(), "kv-log");
  second_cache cache = {-1, ""};
  const struct timespec idle = {0, 5000000L};
  int pending = 0;
  for (;;) {
    // Read stop first: lines committed before it was set are drained in this pass.
    int stop = atomic_load_explicit(&l->stop, memory_order_acquire);
    if (drain(l, &cache) != 0) {
      pending = 1;
      continue;
    }
    if (pending) {
      (void)fflush(l->out);
      pending = 0;
    }
    if (stop) {
      return NULL;
    }
    (void)nanosleep(&idle, NULL);
  }
}

/** Allocate the rings and start the writer thread; free all and return NULL on failure. */
async_log *async_log_new(FILE *out, size_t rings) {
  async_log *l = calloc(1, sizeof(*l));
  if (l == NULL) {
    return NULL;
  }
  // calloc aligns only to 16 bytes; log_ring needs 64. sizeof is a multiple of the alignment.
  l->rings = rings > SIZE_MAX / sizeof(*l->rings)
                 ? NULL
                 : aligned_alloc(_Alignof(log_ring), rings * sizeof(*l->rings));
  if (l->rings == NULL) {
    free(l);
    return NULL;
  }
  memset(l->rings, 0, rings * sizeof(*l->rings));
  for (; l->ring_count != rings; l->ring_count++) {
    if (log_ring_init(&l->rings[l->ring_count], ASYNC_LOG_RING_CAPACITY) != 0) {
      break;
    }
  }
  l->out = out;
  atomic_init(&l->claimed, 0);
  atomic_init(&l->dropped, 0);
  atomic_init(&l->stop, 0);
  l->id = atomic_fetch_add(&next_id, 1);
  if (l->ring_count != rings || pthread_create(&l->writer, NULL, writer_main, l) != 0) {
    for (size_t i = 0; i != l->ring_count; i++) {
      log_ring_free(&l->rings[i]);
    }
    free(l->rings);
    free(l);
    return NULL;
  }
  return l;
}

/** Return a logger whose context is l. */
logger async_log_logger(async_log *l) { return (logger){async_log_write, l}; }

/** Signal the writer to stop, join it after its final drain and flush, then free l. */
void async_log_close(async_log *l) {
  atomic_store_explicit(&l->stop, 1, memory_order_release);
  (void)pthread_join(l->writer, NULL);
  for (size_t i = 0; i != l->ring_count; i++) {
    log_ring_free(&l->rings[i]);
  }
  free(l->rings);
  free(l);
}
