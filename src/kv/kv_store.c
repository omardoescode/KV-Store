#include <stdlib.h>
#include "hashmap/hashmap.h"
#include "kv_store.h"

/**
 * TODO:
 * - Add to struct for persistance
 * - Add to struct for logging
 */
struct kv_store
{
  hashmap *map;
};

kv_store *kv_store_new(void) {
  kv_store *store = calloc(1, sizeof(*store));
  if (!store) return NULL;

  store->map = hashmap_new();
  if (!store->map) {
    free(store);
    return NULL;
  }

  return store;
}

void kv_store_free(kv_store *store) {
  if (!store) return;
  hashmap_free(store->map);
  free(store);
}

int kv_store_get(kv_store *store, const char *key, const char **out_value, size_t *out_len) {
  return hashmap_get(store->map, key, out_value, out_len);
}

int kv_store_put(kv_store *store, const char *key, const char *value, size_t value_len ) {
  return hashmap_put(store->map, key, value, value_len);
}

int kv_store_del(kv_store *store, const char *key) {
  return hashmap_del(store->map, key);
}
