#ifndef KV_STORE_H
#define KV_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "hashmap/hashmap.h"
#include "log/log.h"

typedef struct kv_store kv_store;

/** Called once by kv_store_flush: status 0 when committed, -1 when the store dropped changes. */
typedef void (*kv_flush_fn)(void *ctx, int status);

typedef struct {
  const char *db_path;
  /** >= 1. */
  int flush_interval_ms;
  /** >= 1. */
  size_t max_pending;
  /** >= 1. Seconds a tombstone stays before the purge removes it. */
  long tombstone_ttl_s;
} kv_store_options;

/** One row given by kv_store_dump; the strings are valid only during the callback. */
typedef struct {
  const char *key;
  const void *value;
  size_t value_len;
  uint64_t version;
  bool deleted;
} kv_dump_row;

/** Called per row by kv_store_dump; nonzero stops the dump with an error. */
typedef int (*kv_dump_fn)(void *ctx, const kv_dump_row *row);

/**
 * Load every row of the SQLite database at opt->db_path into memory, adding the version and
 * deleted columns to an older database, then start a writer thread that commits queued changes
 * every flush_interval_ms and, once per second, purges tombstones older than the TTL. The writer
 * thread reports failed commits to log; it keeps the changes and retries them, but drops a change
 * that cannot commit whatever the database state. Put and delete fail while max_pending changes
 * wait for the database. Returns NULL on error.
 */
kv_store *kv_store_new(const kv_store_options *opt, logger log);

/**
 * Stop the writer thread after it commits the queued changes, then free the store. Returns 0, or
 * -1 if the writer dropped changes it could not commit.
 */
int kv_store_free(kv_store *store);

/**
 * Copy key's value into a new buffer the caller must free. Returns 1 on found, 0 on missing or
 * deleted, -1 on error.
 */
int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len);

/**
 * Stores a copy of value under the key with version and queues it for the database, unless the
 * stored version is higher (last writer wins; the put is then ignored). Does no IO.
 * Returns 0 on success or ignore, -1 on error, -2 if max_pending changes wait (the map is
 * unchanged).
 */
int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len,
                 uint64_t version);

/**
 * Replaces the key with a tombstone of version and queues it for the database, unless the
 * stored version is higher (the delete is then ignored). Does no IO.
 * Returns 1 if a value was removed or the delete was ignored, 0 if no value was there, -1 on
 * error, -2 if max_pending changes wait (the map is unchanged).
 */
int kv_store_del(kv_store *store, const char *key, uint64_t version);

/**
 * Remove key and its tombstone, with no version check, and queue the removal. Does no IO.
 * Returns 1 if removed, 0 if missing, -1 on error, -2 if max_pending changes wait.
 */
int kv_store_remove(kv_store *store, const char *key);

/**
 * Call fn for up to limit committed rows, tombstones too, with key > after, in key order.
 * Reads the database, so a change shows only after its commit (see kv_store_flush).
 * Returns the row count, or -1 on error.
 */
int kv_store_dump(kv_store *store, const char *after, size_t limit, kv_dump_fn fn, void *ctx);

/** Pause or resume the tombstone purge. Returns after a running purge round ends. */
void kv_store_pause_purge(kv_store *store, bool paused);

/**
 * Call fn(ctx, status) once every change queued before this call is committed: at once on the
 * calling thread if none waits, else on the writer thread. now asks for a commit before the flush
 * interval ends. Returns 0, or -1 (fn not called) on error or after kv_store_free started.
 */
int kv_store_flush(kv_store *store, bool now, kv_flush_fn fn, void *ctx);

#endif /* KV_STORE_H */
