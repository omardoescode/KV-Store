#ifndef KV_STORE_H
#define KV_STORE_H

#include <pthread.h>
#include "hashmap/hashmap.h"

/**
 * TODO:
- logging
- persistance
 */

typedef struct kv_store kv_store;

kv_store *kv_store_new(void);
void kv_store_free(kv_store *store);

/**
 * Copy key's value into a new buffer the caller must free. Returns 1 on found, 0 on missing.
 */
int kv_store_put(kv_store *store, const char *key, const char value, size_t value_len);

/**
 * Stores a copy of value under the key. Returns 0 on success, -1 on error.
 */
int kv_store_get(kv_store *store, const char *key, const char **out_value, size_t *out_len );

/**
 * Removes the key. Returns 1 if removed, 0 if missing, -1 on error.
 */
int kv_store_del(kv_store *store, const char *key);

#endif /* KV_STORE_H */