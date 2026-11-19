#ifndef KV_STORE_H
#define KV_STORE_H

#include <stdint.h>

#include "hashmap/hashmap.h"
#include "log/log.h"

typedef struct kv_store kv_store;

/** Called once by kv_store_flush: status 0 when committed, -1 when the store dropped changes. */
typedef void (*kv_flush_fn)(void *ctx, int status);

/**
 * Load every row of the SQLite database at db_path into memory, then start a writer thread that
 * commits queued changes every flush_interval_ms (>= 1). The writer thread reports failed
 * commits to log; it keeps the changes and retries them, but drops a change that cannot commit
 * whatever the database state. Put and delete fail while max_pending (>= 1) changes wait for the
 * database. Returns NULL on error.
 */
kv_store *kv_store_new(const char *db_path, int flush_interval_ms, logger log, size_t max_pending);

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

#endif /* KV_STORE_H */
