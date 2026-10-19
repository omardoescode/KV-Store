#include "hashmap/hashmap.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_put_get_del_roundtrip(void) {
  hashmap *m = hashmap_new();
  assert(m);
  assert(hashmap_len(m) == 0);

  assert(hashmap_put(m, "foo", "bar", 3) == 0);
  assert(hashmap_len(m) == 1);

  void *out = NULL;
  size_t out_len = 0;
  assert(hashmap_get(m, "foo", &out, &out_len) == 1);
  assert(out_len == 3);
  assert(memcmp(out, "bar", 3) == 0);
  free(out);

  assert(hashmap_del(m, "foo") == 1);
  assert(hashmap_len(m) == 0);

  out = NULL;
  out_len = 0;
  assert(hashmap_get(m, "foo", &out, &out_len) == 0);
  assert(out == NULL);
  assert(out_len == 0);

  hashmap_free(m);
  printf("  test_put_get_del_roundtrip OK\n");
}

static void test_overwrite(void) {
  hashmap *m = hashmap_new();
  assert(m);

  assert(hashmap_put(m, "k", "short", 5) == 0);
  assert(hashmap_len(m) == 1);
  assert(hashmap_put(m, "k", "a-much-longer-value", 19) == 0);
  assert(hashmap_len(m) == 1);
  assert(hashmap_put(m, "k", "x", 1) == 0);
  assert(hashmap_len(m) == 1);

  void *out = NULL;
  size_t out_len = 0;
  assert(hashmap_get(m, "k", &out, &out_len) == 1);
  assert(out_len == 1);
  assert(memcmp(out, "x", 1) == 0);
  free(out);

  hashmap_free(m);
  printf("  test_overwrite OK\n");
}

static void test_missing_key_get(void) {
  hashmap *m = hashmap_new();
  assert(m);

  void *out = (void *)0xdeadbeef;
  size_t out_len = 99;
  assert(hashmap_get(m, "nope", &out, &out_len) == 0);
  assert(out == NULL);
  assert(out_len == 0);

  assert(hashmap_put(m, "a", "1", 1) == 0);
  assert(hashmap_put(m, "b", "2", 1) == 0);

  out = (void *)0xdeadbeef;
  out_len = 99;
  assert(hashmap_get(m, "c", &out, &out_len) == 0);
  assert(out == NULL);
  assert(out_len == 0);

  hashmap_free(m);
  printf("  test_missing_key_get OK\n");
}

static void test_delete_absent(void) {
  hashmap *m = hashmap_new();
  assert(m);

  assert(hashmap_del(m, "nope") == 0);
  assert(hashmap_len(m) == 0);

  assert(hashmap_put(m, "a", "1", 1) == 0);
  assert(hashmap_del(m, "b") == 0);
  assert(hashmap_len(m) == 1);

  hashmap_free(m);
  printf("  test_delete_absent OK\n");
}

static void test_binary_values(void) {
  hashmap *m = hashmap_new();
  assert(m);

  const unsigned char payload[] = {0x00, 0x01, 0xff, 0x00, 0x7f, 0x80, 0x00};
  assert(hashmap_put(m, "bin", payload, sizeof(payload)) == 0);

  void *out = NULL;
  size_t out_len = 0;
  assert(hashmap_get(m, "bin", &out, &out_len) == 1);
  assert(out_len == sizeof(payload));
  assert(memcmp(out, payload, sizeof(payload)) == 0);
  free(out);

  hashmap_free(m);
  printf("  test_binary_values OK\n");
}

static void test_empty_key_and_empty_value(void) {
  hashmap *m = hashmap_new();
  assert(m);

  assert(hashmap_put(m, "", "val", 3) == 0);
  assert(hashmap_len(m) == 1);

  void *out = NULL;
  size_t out_len = 0;
  assert(hashmap_get(m, "", &out, &out_len) == 1);
  assert(out_len == 3);
  assert(memcmp(out, "val", 3) == 0);
  free(out);

  assert(hashmap_put(m, "zerolen", NULL, 0) == 0);
  assert(hashmap_len(m) == 2);

  out = (void *)0xdeadbeef;
  out_len = 99;
  assert(hashmap_get(m, "zerolen", &out, &out_len) == 1);
  assert(out == NULL);
  assert(out_len == 0);

  hashmap_free(m);
  printf("  test_empty_key_and_empty_value OK\n");
}

static void test_resize_many_inserts(void) {
  hashmap *m = hashmap_new();
  assert(m);

  enum { N = 10000 };
  char key[32];
  char val[32];

  for (int i = 0; i < N; i++) {
    int klen = snprintf(key, sizeof(key), "k%d", i);
    int vlen = snprintf(val, sizeof(val), "v%d", i);
    assert(klen > 0 && vlen > 0);
    assert(hashmap_put(m, key, val, (size_t)vlen) == 0);
  }
  assert(hashmap_len(m) == (size_t)N);

  for (int i = 0; i < N; i++) {
    int klen = snprintf(key, sizeof(key), "k%d", i);
    int vlen = snprintf(val, sizeof(val), "v%d", i);
    assert(klen > 0 && vlen > 0);
    void *out = NULL;
    size_t out_len = 0;
    assert(hashmap_get(m, key, &out, &out_len) == 1);
    assert(out_len == (size_t)vlen);
    assert(memcmp(out, val, (size_t)vlen) == 0);
    free(out);
  }

  for (int i = 0; i < N; i++) {
    int klen = snprintf(key, sizeof(key), "k%d", i);
    assert(klen > 0);
    assert(hashmap_del(m, key) == 1);
  }
  assert(hashmap_len(m) == 0);

  hashmap_free(m);
  printf("  test_resize_many_inserts OK\n");
}

enum { NUM_WRITERS = 4, NUM_READERS = 4, OPS_PER_WRITER = 25000 };

typedef struct {
  hashmap *m;
  int tid;
} writer_arg;

typedef struct {
  hashmap *m;
  _Atomic int *stop;
  uint64_t gets_done;
} reader_arg;

static void *writer_thread(void *p) {
  writer_arg *a = p;
  char key[32];
  char val[32];
  for (int i = 0; i < OPS_PER_WRITER; i++) {
    int klen = snprintf(key, sizeof(key), "t%d_%d", a->tid, i);
    int vlen = snprintf(val, sizeof(val), "v%d_%d", a->tid, i);
    assert(klen > 0 && vlen > 0);
    int rc = hashmap_put(a->m, key, val, (size_t)vlen);
    assert(rc == 0);
  }
  return NULL;
}

static void *reader_thread(void *p) {
  reader_arg *a = p;
  char key[32];
  uint64_t n = 0;
  while (!atomic_load(a->stop)) {
    int tid = (int)(n % NUM_WRITERS);
    int i = (int)(n % OPS_PER_WRITER);
    int klen = snprintf(key, sizeof(key), "t%d_%d", tid, i);
    assert(klen > 0);
    void *out = NULL;
    size_t out_len = 0;
    int rc = hashmap_get(a->m, key, &out, &out_len);
    assert(rc == 0 || rc == 1);
    if (out) {
      free(out);
    }
    (void)hashmap_len(a->m);
    n++;
  }
  a->gets_done = n;
  return NULL;
}

static void test_concurrent(void) {
  hashmap *m = hashmap_new();
  assert(m);

  pthread_t writers[NUM_WRITERS];
  pthread_t readers[NUM_READERS];
  writer_arg wargs[NUM_WRITERS];
  reader_arg rargs[NUM_READERS];
  _Atomic int stop = 0;

  for (int i = 0; i < NUM_READERS; i++) {
    rargs[i].m = m;
    rargs[i].stop = &stop;
    rargs[i].gets_done = 0;
    assert(pthread_create(&readers[i], NULL, reader_thread, &rargs[i]) == 0);
  }
  for (int i = 0; i < NUM_WRITERS; i++) {
    wargs[i].m = m;
    wargs[i].tid = i;
    assert(pthread_create(&writers[i], NULL, writer_thread, &wargs[i]) == 0);
  }

  for (int i = 0; i < NUM_WRITERS; i++) {
    assert(pthread_join(writers[i], NULL) == 0);
  }
  atomic_store(&stop, 1);
  uint64_t total_reads = 0;
  for (int i = 0; i < NUM_READERS; i++) {
    assert(pthread_join(readers[i], NULL) == 0);
    total_reads += rargs[i].gets_done;
  }

  assert(hashmap_len(m) == (size_t)(NUM_WRITERS * OPS_PER_WRITER));

  char key[32];
  char val[32];
  for (int tid = 0; tid < NUM_WRITERS; tid++) {
    for (int i = 0; i < OPS_PER_WRITER; i++) {
      int klen = snprintf(key, sizeof(key), "t%d_%d", tid, i);
      int vlen = snprintf(val, sizeof(val), "v%d_%d", tid, i);
      assert(klen > 0 && vlen > 0);
      void *out = NULL;
      size_t out_len = 0;
      assert(hashmap_get(m, key, &out, &out_len) == 1);
      assert(out_len == (size_t)vlen);
      assert(memcmp(out, val, (size_t)vlen) == 0);
      free(out);
    }
  }

  for (int tid = 0; tid < NUM_WRITERS; tid++) {
    for (int i = 0; i < OPS_PER_WRITER; i++) {
      int klen = snprintf(key, sizeof(key), "t%d_%d", tid, i);
      assert(klen > 0);
      assert(hashmap_del(m, key) == 1);
    }
  }
  assert(hashmap_len(m) == 0);

  hashmap_free(m);
  printf("  test_concurrent OK (%llu reads during writes)\n",
         (unsigned long long)total_reads);
}

int main(void) {
  printf("[hashmap_test] running\n");
  test_put_get_del_roundtrip();
  test_overwrite();
  test_missing_key_get();
  test_delete_absent();
  test_binary_values();
  test_empty_key_and_empty_value();
  test_resize_many_inserts();
  test_concurrent();
  printf("[hashmap_test] PASS\n");
  return 0;
}
