#ifndef KV_STORE_H
#define KV_STORE_H

#include "hashmap/hashmap.h"
#include "log/log.h"

typedef struct kv_store kv_store;

/**
 * Load every row of the SQLite database at db_path into memory, then start a writer thread that
 * commits queued changes every flush_interval_ms (>= 1). The writer thread reports failed
 * commits to log; it keeps the changes and retries them, but drops a change that cannot commit
 * whatever the database state. Returns NULL on error.
 */
kv_store *kv_store_new(const char *db_path, int flush_interval_ms, logger log);

/**
 * Stop the writer thread after it commits the queued changes, then free the store. Returns 0, or
 * -1 if the writer dropped changes it could not commit.
 */
int kv_store_free(kv_store *store);

/**
 * Copy key's value into a new buffer the caller must free. Returns 1 on found, 0 on missing,
 * -1 on error.
 */
int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len);

/**
 * Stores a copy of value under the key and queues it for the database. Does no IO.
 * Returns 0 on success, -1 on error.
 */
int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len);

/**
 * Removes the key and queues the removal for the database. Does no IO.
 * Returns 1 if removed, 0 if missing, -1 on error.
 */
int kv_store_del(kv_store *store, const char *key);

#endif /* KV_STORE_H */
