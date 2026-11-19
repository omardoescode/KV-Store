#include "kv_store.h"
#include "hashmap/hashmap.h"
#include <errno.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
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

/** The prefix of each map value; the value bytes follow it. deleted: unix ms, 0 if live. */
typedef struct {
  uint64_t version;
  int64_t deleted;
} entry_head;

/** One queued put (deleted 0) or tombstone, in one allocation: key, then '\0', then value. */
typedef struct change {
  struct change *next;
  // Removes the row instead of writing it.
  bool remove;
  uint64_t version;
  // Unix ms of the delete, 0 for a put.
  int64_t deleted;
  size_t value_len;
  char *value;
  char key[];
} change;

typedef struct flush_waiter {
  struct flush_waiter *next;
  // Fires once every change up to this sequence number is committed.
  uint64_t seq;
  kv_flush_fn fn;
  void *ctx;
} flush_waiter;

struct kv_store {
  hashmap *map;
  logger log;
  int flush_interval_ms;
  int64_t tombstone_ttl_ms;
  sqlite3 *db;
  sqlite3_stmt *upsert;
  sqlite3_stmt *remove;
  sqlite3_stmt *purge_select;
  sqlite3_stmt *purge_delete;
  // A second connection for kv_store_dump, which runs on caller threads. Guarded by reader_mu.
  sqlite3 *reader;
  sqlite3_stmt *dump;
  pthread_mutex_t reader_mu;
  // Held for a whole purge round, so kv_store_pause_purge returns only when none runs.
  pthread_mutex_t purge_mu;
  bool purge_paused;
  int64_t last_purge_ms;
  pthread_t writer;
  pthread_mutex_t mu;
  pthread_cond_t wake;
  // Changes not taken by the writer yet, oldest first. Guarded by mu.
  change *head;
  change **tail;
  size_t queued;
  // Changes the writer holds in batch, as of its last lock of mu. Guarded by mu.
  size_t in_batch;
  size_t max_pending;
  bool stop;
  // Sequence number of the last queued change, and of the last committed one. Guarded by mu.
  uint64_t last_seq;
  uint64_t committed_seq;
  // Flush waiters not fired yet. Guarded by mu.
  flush_waiter *waiters;
  // Changes the writer took but did not commit yet, oldest first. Writer thread only.
  change *batch;
  change **batch_tail;
  size_t batch_len;
  // Changes the writer dropped. Writer thread only until it is joined.
  size_t dropped;
};

static change *change_new(const char *key, const void *value, size_t value_len, entry_head head) {
  size_t key_len = strlen(key);
  change *c = malloc(sizeof(*c) + key_len + 1 + value_len);
  if (!c) {
    return NULL;
  }
  c->next = NULL;
  c->remove = false;
  c->version = head.version;
  c->deleted = head.deleted;
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
  store->last_seq++;
  *store->tail = c;
  store->tail = &c->next;
  if (++store->queued == KV_STORE_WAKE_LEN) {
    pthread_cond_signal(&store->wake);
  }
}

/** Put head and value_len bytes of value under key in the map. Returns 0, or -1 on error. */
static int map_put(hashmap *map, const char *key, entry_head head, const void *value,
                   size_t value_len) {
  unsigned char stack[256];
  size_t len = sizeof(head) + value_len;
  unsigned char *buf = len <= sizeof(stack) ? stack : malloc(len);
  if (!buf) {
    return -1;
  }
  memcpy(buf, &head, sizeof(head));
  if (value_len) {
    memcpy(buf + sizeof(head), value, value_len);
  }
  int rc = hashmap_put(map, key, buf, len);
  if (buf != stack) {
    free(buf);
  }
  return rc;
}

/**
 * Read the head of key's map entry into *head. Returns 1 if found, 0 if missing, -1 on error.
 * If value is not NULL, set *value to a malloc buffer that holds the value at its start.
 */
static int map_get(hashmap *map, const char *key, entry_head *head, void **value,
                   size_t *value_len) {
  void *raw = NULL;
  size_t len = 0;
  int rc = hashmap_get(map, key, &raw, &len);
  if (rc != 1) {
    return rc;
  }
  if (len < sizeof(*head)) {
    free(raw);
    return -1;
  }
  memcpy(head, raw, sizeof(*head));
  if (value) {
    *value_len = len - sizeof(*head);
    memmove(raw, (unsigned char *)raw + sizeof(*head), *value_len);
    *value = raw;
  } else {
    free(raw);
  }
  return 1;
}

static int64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ((int64_t)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
}

static int load_rows(kv_store *store) {
  sqlite3_stmt *select = NULL;
  if (sqlite3_prepare_v2(store->db, "SELECT key, value, version, deleted FROM kv", -1, &select,
                         NULL) != SQLITE_OK) {
    return -1;
  }
  int rc;
  while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
    const unsigned char *key = sqlite3_column_text(select, 0);
    const void *value = sqlite3_column_blob(select, 1);
    size_t value_len = (size_t)sqlite3_column_bytes(select, 1);
    entry_head head = {(uint64_t)sqlite3_column_int64(select, 2), sqlite3_column_int64(select, 3)};
    if (!key || map_put(store->map, (const char *)key, head, value, value_len) != 0) {
      break;
    }
  }
  sqlite3_finalize(select);
  return rc == SQLITE_DONE ? 0 : -1;
}

/** Add the version and deleted columns to a kv table made before they existed. */
static int upgrade_schema(sqlite3 *db) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM pragma_table_info('kv') WHERE name = 'version'",
                         -1, &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  int rc = sqlite3_step(stmt);
  int has_version = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) == 1;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) {
    return -1;
  }
  if (has_version) {
    return 0;
  }
  return sqlite3_exec(db,
                      "BEGIN;"
                      "ALTER TABLE kv ADD COLUMN version INTEGER NOT NULL DEFAULT 0;"
                      "ALTER TABLE kv ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0;"
                      "COMMIT;",
                      NULL, NULL, NULL) == SQLITE_OK
             ? 0
             : -1;
}

/**
 * Return whether rc comes from the data of one change, so that change fails whatever the database
 * state. Other errors (a lock, a full disk, a dropped table, corruption) keep the whole batch.
 */
static bool poison(int rc) {
  switch (rc) {
  case SQLITE_TOOBIG:
  case SQLITE_CONSTRAINT:
  case SQLITE_MISMATCH:
    return true;
  default:
    return false;
  }
}

/** Bind the columns of c to stmt, the remove or upsert statement. Returns an SQLite code. */
static int bind_change(sqlite3_stmt *stmt, const change *c) {
  int rc = sqlite3_bind_text(stmt, 1, c->key, -1, SQLITE_STATIC);
  if (c->remove) {
    return rc;
  }
  if (rc == SQLITE_OK) {
    // c->value is never NULL, so an empty value binds as an empty blob, not NULL.
    rc = sqlite3_bind_blob64(stmt, 2, c->value, c->value_len, SQLITE_STATIC);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)c->version);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(stmt, 4, c->deleted);
  }
  return rc;
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
    sqlite3_stmt *stmt = c->remove ? store->remove : store->upsert;
    rc = bind_change(stmt, c);
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
  if (!failed || !poison(rc)) {
    return -1;
  }
  change *bad = *failed;
  store->log.log(store->log.ctx, LOG_ERROR, "kv_store: dropping the change that cannot commit");
  *failed = bad->next;
  if (!bad->next) {
    store->batch_tail = failed;
  }
  store->batch_len--;
  store->dropped++;
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
    store->in_batch = store->batch_len;
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
        store->dropped += store->batch_len;
        return NULL;
      }
      nanosleep(&final_retry, NULL);
    }
    pthread_mutex_lock(&store->mu);
    store->in_batch = store->batch_len;
  }
}

kv_store *kv_store_new(const char *db_path, int flush_interval_ms, logger log, size_t max_pending) {
  kv_store *store = calloc(1, sizeof(*store));
  if (!store) {
    return NULL;
  }
  store->log = log;
  store->flush_interval_ms = flush_interval_ms;
  store->max_pending = max_pending;
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
                   "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value BLOB,"
                   " version INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0)"
                   " WITHOUT ROWID;",
                   NULL, NULL, NULL) != SQLITE_OK ||
      upgrade_schema(store->db) != 0 ||
      sqlite3_prepare_v2(store->db,
                         "INSERT INTO kv(key, value, version, deleted) VALUES(?1, ?2, ?3, ?4)"
                         " ON CONFLICT(key) DO UPDATE SET value = excluded.value,"
                         " version = excluded.version, deleted = excluded.deleted",
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

  int status = store->dropped ? -1 : 0;
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
  entry_head head;
  void *value = NULL;
  size_t len = 0;
  int rc = map_get(store->map, key, &head, &value, &len);
  if (rc != 1 || head.deleted) {
    free(value);
    *out_value = NULL;
    *out_len = 0;
    return rc == 1 ? 0 : rc;
  }
  if (len == 0) {
    free(value);
    value = NULL;
  }
  *out_value = value;
  *out_len = len;
  return 1;
}

/**
 * Apply a put (deleted 0) or tombstone with version to the map and queue it, unless the stored
 * version is higher. Map and queue change under one lock so the queue holds changes in map order,
 * and the database ends with the entry the map holds. Returns 1 if applied and a live value was
 * there before, 0 if applied and none was, 2 if ignored, -1 on error, -2 if max_pending changes
 * wait (the map is unchanged).
 */
static int apply(kv_store *store, const char *key, const void *value, size_t value_len,
                 entry_head head) {
  change *c = change_new(key, value, value_len, head);
  if (!c) {
    return -1;
  }
  pthread_mutex_lock(&store->mu);
  entry_head old = {0, 0};
  int status = -2;
  if (store->queued + store->in_batch < store->max_pending) {
    int found = map_get(store->map, key, &old, NULL, NULL);
    if (found < 0) {
      status = -1;
    } else if (found && head.version < old.version) {
      status = 2;
    } else {
      status = found && !old.deleted ? 1 : 0;
      if (map_put(store->map, key, head, value, value_len) != 0) {
        status = -1;
      }
    }
  }
  if (status == 0 || status == 1) {
    enqueue(store, c);
  }
  pthread_mutex_unlock(&store->mu);
  if (status != 0 && status != 1) {
    free(c);
  }
  return status;
}

int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len,
                 uint64_t version) {
  int rc = apply(store, key, value, value_len, (entry_head){version, 0});
  return rc >= 0 ? 0 : rc;
}

int kv_store_del(kv_store *store, const char *key, uint64_t version) {
  int rc = apply(store, key, NULL, 0, (entry_head){version, now_ms()});
  return rc == 2 ? 1 : rc;
}
