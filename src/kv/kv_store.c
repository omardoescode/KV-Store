#include "kv_store.h"
#include "hashmap/hashmap.h"
#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Queue length that wakes the writer before its interval ends, to bound the batch size.
#define KV_STORE_WAKE_LEN 4096
// Commit attempts at shutdown before the writer drops the changes it could not commit.
#define KV_STORE_FINAL_TRIES 10
// Longest pause between shutdown commit attempts, so a long flush interval cannot stall exit.
#define KV_STORE_FINAL_RETRY_MS 100

/** One queued put (deleted false) or delete, in one allocation: key, then '\0', then value. */
typedef struct change {
  struct change *next;
  bool deleted;
  size_t value_len;
  char *value;
  char key[];
} change;

struct kv_store {
  hashmap *map;
  logger log;
  int flush_interval_ms;
  sqlite3 *db;
  sqlite3_stmt *upsert;
  sqlite3_stmt *remove;
  pthread_t writer;
  pthread_mutex_t mu;
  pthread_cond_t wake;
  // Changes not taken by the writer yet, oldest first. Guarded by mu.
  change *head;
  change **tail;
  size_t queued;
  bool stop;
  // Changes the writer took but did not commit yet, oldest first. Writer thread only.
  change *batch;
  change **batch_tail;
  size_t batch_len;
};

static change *change_new(const char *key, const void *value, size_t value_len, bool deleted) {
  size_t key_len = strlen(key);
  change *c = malloc(sizeof(*c) + key_len + 1 + value_len);
  if (!c) {
    return NULL;
  }
  c->next = NULL;
  c->deleted = deleted;
  c->value_len = value_len;
  memcpy(c->key, key, key_len + 1);
  c->value = c->key + key_len + 1;
  if (value_len) {
    memcpy(c->value, value, value_len);
  }
  return c;
}

static void change_free_all(change *c) {
  while (c) {
    change *next = c->next;
    free(c);
    c = next;
  }
}

/** Append c to the queue. Call with mu held. */
static void enqueue(kv_store *store, change *c) {
  *store->tail = c;
  store->tail = &c->next;
  if (++store->queued == KV_STORE_WAKE_LEN) {
    pthread_cond_signal(&store->wake);
  }
}

static int load_rows(kv_store *store) {
  sqlite3_stmt *select = NULL;
  if (sqlite3_prepare_v2(store->db, "SELECT key, value FROM kv", -1, &select, NULL) != SQLITE_OK) {
    return -1;
  }
  int rc;
  while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
    const unsigned char *key = sqlite3_column_text(select, 0);
    const void *value = sqlite3_column_blob(select, 1);
    size_t value_len = (size_t)sqlite3_column_bytes(select, 1);
    if (!key || hashmap_put(store->map, (const char *)key, value, value_len) != 0) {
      break;
    }
  }
  sqlite3_finalize(select);
  return rc == SQLITE_DONE ? 0 : -1;
}

/** Return whether rc can clear without a change to the data, as a lock or a full disk can. */
static bool transient(int rc) {
  switch (rc) {
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
  case SQLITE_NOMEM:
  case SQLITE_READONLY:
  case SQLITE_INTERRUPT:
  case SQLITE_IOERR:
  case SQLITE_FULL:
  case SQLITE_CANTOPEN:
    return true;
  default:
    return false;
  }
}

/**
 * Commit the batch in one transaction and return 0. On error, log it, roll back, and return -1;
 * or return 1 if the error came from one change that cannot succeed, after it drops that change.
 */
static int commit_batch(kv_store *store) {
  int rc = sqlite3_exec(store->db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
  change **failed = NULL;
  for (change **link = &store->batch; *link && rc == SQLITE_OK; link = &(*link)->next) {
    change *c = *link;
    sqlite3_stmt *stmt = c->deleted ? store->remove : store->upsert;
    rc = sqlite3_bind_text(stmt, 1, c->key, -1, SQLITE_STATIC);
    if (rc == SQLITE_OK && !c->deleted) {
      // c->value is never NULL, so an empty value binds as an empty blob, not NULL.
      rc = sqlite3_bind_blob64(stmt, 2, c->value, c->value_len, SQLITE_STATIC);
    }
    if (rc == SQLITE_OK) {
      rc = sqlite3_step(stmt);
      rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
    }
    if (rc != SQLITE_OK) {
      failed = link;
    }
    sqlite3_reset(stmt);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_exec(store->db, "COMMIT", NULL, NULL, NULL);
  }
  if (rc == SQLITE_OK) {
    return 0;
  }
  store->log.log(store->log.ctx, LOG_ERROR, "kv_store: commit of %zu changes failed: %s",
                 store->batch_len, sqlite3_errmsg(store->db));
  if (!sqlite3_get_autocommit(store->db)) {
    (void)sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
  }
  if (!failed || transient(rc)) {
    return -1;
  }
  change *bad = *failed;
  store->log.log(store->log.ctx, LOG_ERROR, "kv_store: dropping the change that cannot commit");
  *failed = bad->next;
  if (!bad->next) {
    store->batch_tail = failed;
  }
  store->batch_len--;
  free(bad);
  return 1;
}

/** Commit queued changes every flush interval, keeping a failed batch to retry it. */
static void *writer_main(void *arg) {
  kv_store *store = arg;
  pthread_setname_np(pthread_self(), "kv-writer");
  struct timespec interval = {
      .tv_sec = store->flush_interval_ms / 1000,
      .tv_nsec = (long)(store->flush_interval_ms % 1000) * 1000000,
  };
  struct timespec final_retry = interval;
  if (store->flush_interval_ms > KV_STORE_FINAL_RETRY_MS) {
    final_retry = (struct timespec){.tv_sec = 0, .tv_nsec = KV_STORE_FINAL_RETRY_MS * 1000000L};
  }
  int final_tries = 0;
  pthread_mutex_lock(&store->mu);
  for (;;) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += interval.tv_sec;
    deadline.tv_nsec += interval.tv_nsec;
    if (deadline.tv_nsec >= 1000000000) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }
    while (!store->stop && store->queued < KV_STORE_WAKE_LEN &&
           pthread_cond_timedwait(&store->wake, &store->mu, &deadline) != ETIMEDOUT) {
    }
    bool stopping = store->stop;
    if (store->head) {
      *store->batch_tail = store->head;
      store->batch_tail = store->tail;
      store->batch_len += store->queued;
      store->head = NULL;
      store->tail = &store->head;
      store->queued = 0;
    }
    pthread_mutex_unlock(&store->mu);

    int rc = -1;
    while (store->batch && (rc = commit_batch(store)) == 1) {
    }
    if (rc == 0) {
      change_free_all(store->batch);
      store->batch = NULL;
      store->batch_tail = &store->batch;
      store->batch_len = 0;
    }
    if (stopping) {
      if (!store->batch) {
        return NULL;
      }
      if (++final_tries == KV_STORE_FINAL_TRIES) {
        store->log.log(store->log.ctx, LOG_ERROR, "kv_store: dropping %zu uncommitted changes",
                       store->batch_len);
        return NULL;
      }
      nanosleep(&final_retry, NULL);
    }
    pthread_mutex_lock(&store->mu);
  }
}

kv_store *kv_store_new(const char *db_path, int flush_interval_ms, logger log) {
  kv_store *store = calloc(1, sizeof(*store));
  if (!store) {
    return NULL;
  }
  store->log = log;
  store->flush_interval_ms = flush_interval_ms;
  store->tail = &store->head;
  store->batch_tail = &store->batch;

  pthread_condattr_t attr;
  if (pthread_condattr_init(&attr) != 0) {
    free(store);
    return NULL;
  }
  int sync_ok = pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) == 0 &&
                pthread_cond_init(&store->wake, &attr) == 0;
  pthread_condattr_destroy(&attr);
  if (!sync_ok) {
    free(store);
    return NULL;
  }
  if (pthread_mutex_init(&store->mu, NULL) != 0) {
    pthread_cond_destroy(&store->wake);
    free(store);
    return NULL;
  }

  store->map = hashmap_new();
  if (!store->map) {
    goto fail;
  }
  if (sqlite3_open(db_path, &store->db) != SQLITE_OK ||
      sqlite3_exec(store->db,
                   "PRAGMA journal_mode=WAL;"
                   "PRAGMA synchronous=FULL;"
                   "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value BLOB) WITHOUT ROWID;",
                   NULL, NULL, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(store->db,
                         "INSERT INTO kv(key, value) VALUES(?1, ?2)"
                         " ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                         -1, &store->upsert, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(store->db, "DELETE FROM kv WHERE key = ?1", -1, &store->remove, NULL) !=
          SQLITE_OK ||
      load_rows(store) != 0) {
    (void)fprintf(stderr, "kv_store: could not load %s: %s\n", db_path,
                  store->db ? sqlite3_errmsg(store->db) : "out of memory");
    goto fail;
  }

  if (pthread_create(&store->writer, NULL, writer_main, store) != 0) {
    goto fail;
  }
  return store;

fail:
  sqlite3_finalize(store->upsert);
  sqlite3_finalize(store->remove);
  sqlite3_close(store->db);
  hashmap_free(store->map);
  pthread_mutex_destroy(&store->mu);
  pthread_cond_destroy(&store->wake);
  free(store);
  return NULL;
}

int kv_store_free(kv_store *store) {
  if (!store) {
    return 0;
  }
  pthread_mutex_lock(&store->mu);
  store->stop = true;
  pthread_cond_signal(&store->wake);
  pthread_mutex_unlock(&store->mu);
  pthread_join(store->writer, NULL);

  int status = store->batch ? -1 : 0;
  change_free_all(store->head);
  change_free_all(store->batch);
  sqlite3_finalize(store->upsert);
  sqlite3_finalize(store->remove);
  sqlite3_close(store->db);
  hashmap_free(store->map);
  pthread_mutex_destroy(&store->mu);
  pthread_cond_destroy(&store->wake);
  free(store);
  return status;
}

int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len) {
  return hashmap_get(store->map, key, out_value, out_len);
}

// Map and queue change under one lock so the queue holds changes in map order, and the database
// ends with the value the map holds.
int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len) {
  change *c = change_new(key, value, value_len, false);
  if (!c) {
    return -1;
  }
  pthread_mutex_lock(&store->mu);
  int status = hashmap_put(store->map, key, value, value_len);
  if (status == 0) {
    enqueue(store, c);
  }
  pthread_mutex_unlock(&store->mu);
  if (status != 0) {
    free(c);
  }
  return status;
}

int kv_store_del(kv_store *store, const char *key) {
  change *c = change_new(key, NULL, 0, true);
  if (!c) {
    return -1;
  }
  pthread_mutex_lock(&store->mu);
  int status = hashmap_del(store->map, key);
  if (status == 1) {
    enqueue(store, c);
  }
  pthread_mutex_unlock(&store->mu);
  if (status != 1) {
    free(c);
  }
  return status;
}
