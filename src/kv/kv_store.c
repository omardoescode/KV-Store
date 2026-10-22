#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hashmap/hashmap.h"
#include "kv_store.h"

struct kv_store
{
  hashmap *map;
  char *persistence_path;
  pthread_t bckgrd_thread;
  atomic_bool dirty_flag;
  atomic_bool stop;
};

// Write one record to the file.
static int write_record(const char *key, size_t key_len, const void *val, size_t val_len, void *arg) {
  FILE *f = arg;
  if (fwrite(key, 1, key_len, f) != key_len || fputc(' ', f) == EOF) return -1;
  if (val_len && fwrite(val, 1, val_len, f) != val_len) return -1;
  if (fputc('\n', f) == EOF) return -1;
  return 0;
}

// Load the snapshot into the map
static int kv_store_load(kv_store *store) {
  FILE *f = fopen(store->persistence_path, "rb");
  if (!f) return errno == ENOENT ? 0 : -1;

  int rc = 0;
  char *line = NULL;
  size_t cap = 0;
  ssize_t n;
  while ((n = getline(&line, &cap, f)) != -1) {
    if (n > 0 && line[n - 1] == '\n') line[--n] = '\0';

    char *sep = memchr(line, ' ', (size_t)n);
    if (!sep || sep == line) {
      rc = -1;
      break;
    }
    *sep = '\0';
    const char *val = sep + 1;
    size_t val_len = (size_t)(line + n - val);

    if (hashmap_put(store->map, line, val, val_len) != 0) {
      rc = -1;
      break;
    }
  }

  free(line);
  fclose(f);
  return rc;
}

int kv_store_save(kv_store *store) {
  char temp[4096];
  snprintf(temp, sizeof(temp), "%s.temp", store->persistence_path);

  FILE *f = fopen(temp, "wb");
  if (!f) return -1;
  int ok = hashmap_foreach(store->map, write_record, f) == 0;
  ok = (fflush(f) == 0) && ok; // stdio buffer -> OS
  ok = (fsync(fileno(f)) == 0) && ok; // OS cache -> disk
  ok = (fclose(f) == 0) && ok;

  if (ok && rename(temp, store->persistence_path) == 0) return 0;
  remove(temp);
  return -1;
}

// Background thread that saves to the snapshot file if anything changed.
static void *snapshot_loop(void *arg) {
  kv_store *store = arg;
  while (!atomic_load(&store->stop)) {
    usleep(100000);
    if (atomic_exchange(&store->dirty_flag, false)) kv_store_save(store);
  }
  return NULL;
}

kv_store *kv_store_new(const char *persistence_path) {
  kv_store *store = calloc(1, sizeof(*store));
  if (!store) return NULL;

  store->map = hashmap_new();
  store->persistence_path = strdup(persistence_path);
  if (!store->map || !store->persistence_path) goto fail;

  if (kv_store_load(store) != 0) {
    fprintf(stderr, "kv_store: could not load snapshot %s\n", persistence_path);
    goto fail;
  }

  if (pthread_create(&store->bckgrd_thread, NULL, snapshot_loop, store) != 0) goto fail;
  return store;

fail:
  hashmap_free(store->map);
  free(store->persistence_path);
  free(store);
  return NULL;
}

void kv_store_free(kv_store *store) {
  if (!store) return;
  atomic_store(&store->stop, true);
  pthread_join(store->bckgrd_thread, NULL);
  if (atomic_load(&store->dirty_flag)) kv_store_save(store); // final snapshot
  hashmap_free(store->map);
  free(store->persistence_path);
  free(store);
}

int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len) {
  return hashmap_get(store->map, key, out_value, out_len);
}

int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len ) {
  int status = hashmap_put(store->map, key, value, value_len);
  if (status == 0) store->dirty_flag = true;
  return status;
}

int kv_store_del(kv_store *store, const char *key) {
  int status = hashmap_del(store->map, key);
  if (status == 1) store->dirty_flag = true;
  return status;
}
