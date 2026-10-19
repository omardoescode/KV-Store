#include "hashmap/hashmap.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HM_INITIAL_BUCKETS 16u
#define HM_LOAD_NUM 3u
#define HM_LOAD_DEN 4u

typedef struct hm_entry {
  struct hm_entry *next;
  uint64_t hash;
  char *key;
  void *val;
  size_t key_len;
  size_t val_len;
} hm_entry;

struct hashmap {
  hm_entry **buckets;
  size_t bucket_count;
  size_t count;
  pthread_rwlock_t lock;
};

static uint64_t hm_hash(const char *key, size_t key_len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < key_len; i++) {
    h ^= (uint64_t)(unsigned char)key[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static size_t hm_idx(uint64_t h, size_t bucket_count) {
  return (size_t)(h & (uint64_t)(bucket_count - 1u));
}

static hm_entry *hm_find(const hashmap *m, const char *key, size_t key_len, uint64_t h,
                        size_t *out_idx) {
  size_t idx = hm_idx(h, m->bucket_count);
  if (out_idx) {
    *out_idx = idx;
  }
  for (hm_entry *e = m->buckets[idx]; e; e = e->next) {
    if (e->hash == h && e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
      return e;
    }
  }
  return NULL;
}

// Assumes the write lock is held. Doubles the bucket array and rehashes.
static int hm_resize_locked(hashmap *m) {
  size_t new_count = m->bucket_count * 2u;
  hm_entry **new_buckets = calloc(new_count, sizeof(*new_buckets));
  if (!new_buckets) {
    return -1;
  }
  for (size_t i = 0; i < m->bucket_count; i++) {
    hm_entry *e = m->buckets[i];
    while (e) {
      hm_entry *next = e->next;
      size_t idx = hm_idx(e->hash, new_count);
      e->next = new_buckets[idx];
      new_buckets[idx] = e;
      e = next;
    }
  }
  free(m->buckets);
  m->buckets = new_buckets;
  m->bucket_count = new_count;
  return 0;
}

hashmap *hashmap_new(void) {
  hashmap *m = calloc(1, sizeof(*m));
  if (!m) {
    return NULL;
  }
  m->buckets = calloc(HM_INITIAL_BUCKETS, sizeof(*m->buckets));
  if (!m->buckets) {
    free(m);
    return NULL;
  }
  m->bucket_count = HM_INITIAL_BUCKETS;
  m->count = 0;
  if (pthread_rwlock_init(&m->lock, NULL) != 0) {
    free(m->buckets);
    free(m);
    return NULL;
  }
  return m;
}

void hashmap_free(hashmap *m) {
  if (!m) {
    return;
  }
  for (size_t i = 0; i < m->bucket_count; i++) {
    hm_entry *e = m->buckets[i];
    while (e) {
      hm_entry *next = e->next;
      free(e->key);
      free(e->val);
      free(e);
      e = next;
    }
  }
  free(m->buckets);
  pthread_rwlock_destroy(&m->lock);
  free(m);
}

int hashmap_put(hashmap *m, const char *key, const void *val, size_t val_len) {
  if (!m || !key || (val_len > 0 && !val)) {
    return -1;
  }
  size_t key_len = strlen(key);
  uint64_t h = hm_hash(key, key_len);

  pthread_rwlock_wrlock(&m->lock);

  size_t idx;
  hm_entry *existing = hm_find(m, key, key_len, h, &idx);

  void *new_val = NULL;
  if (val_len > 0) {
    new_val = malloc(val_len);
    if (!new_val) {
      pthread_rwlock_unlock(&m->lock);
      return -1;
    }
    memcpy(new_val, val, val_len);
  }

  if (existing) {
    free(existing->val);
    existing->val = new_val;
    existing->val_len = val_len;
    pthread_rwlock_unlock(&m->lock);
    return 0;
  }

  char *key_copy = malloc(key_len + 1);
  if (!key_copy) {
    free(new_val);
    pthread_rwlock_unlock(&m->lock);
    return -1;
  }
  memcpy(key_copy, key, key_len + 1);

  hm_entry *e = malloc(sizeof(*e));
  if (!e) {
    free(key_copy);
    free(new_val);
    pthread_rwlock_unlock(&m->lock);
    return -1;
  }
  e->hash = h;
  e->key = key_copy;
  e->key_len = key_len;
  e->val = new_val;
  e->val_len = val_len;
  e->next = m->buckets[idx];
  m->buckets[idx] = e;
  m->count++;

  if (m->count * HM_LOAD_DEN > m->bucket_count * HM_LOAD_NUM) {
    // Resize failure leaves the map correct, just over the target load factor.
    (void)hm_resize_locked(m);
  }

  pthread_rwlock_unlock(&m->lock);
  return 0;
}

int hashmap_get(const hashmap *m, const char *key, void **out_val, size_t *out_len) {
  if (!m || !key || !out_val || !out_len) {
    return -1;
  }
  *out_val = NULL;
  *out_len = 0;
  size_t key_len = strlen(key);
  uint64_t h = hm_hash(key, key_len);

  // Cast away const to lock: the lock is a mutable part of a logically-const object.
  hashmap *mm = (hashmap *)m;
  pthread_rwlock_rdlock(&mm->lock);

  hm_entry *e = hm_find(m, key, key_len, h, NULL);
  if (!e) {
    pthread_rwlock_unlock(&mm->lock);
    return 0;
  }

  void *copy = NULL;
  if (e->val_len > 0) {
    copy = malloc(e->val_len);
    if (!copy) {
      pthread_rwlock_unlock(&mm->lock);
      return -1;
    }
    memcpy(copy, e->val, e->val_len);
  }
  *out_val = copy;
  *out_len = e->val_len;
  pthread_rwlock_unlock(&mm->lock);
  return 1;
}

int hashmap_del(hashmap *m, const char *key) {
  if (!m || !key) {
    return -1;
  }
  size_t key_len = strlen(key);
  uint64_t h = hm_hash(key, key_len);

  pthread_rwlock_wrlock(&m->lock);

  size_t idx = hm_idx(h, m->bucket_count);
  hm_entry **slot = &m->buckets[idx];
  while (*slot) {
    hm_entry *e = *slot;
    if (e->hash == h && e->key_len == key_len && memcmp(e->key, key, key_len) == 0) {
      *slot = e->next;
      free(e->key);
      free(e->val);
      free(e);
      m->count--;
      pthread_rwlock_unlock(&m->lock);
      return 1;
    }
    slot = &e->next;
  }

  pthread_rwlock_unlock(&m->lock);
  return 0;
}

size_t hashmap_len(const hashmap *m) {
  if (!m) {
    return 0;
  }
  hashmap *mm = (hashmap *)m;
  pthread_rwlock_rdlock(&mm->lock);
  size_t n = m->count;
  pthread_rwlock_unlock(&mm->lock);
  return n;
}
