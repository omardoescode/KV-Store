#include "kv_store.h"
#include "hashmap/hashmap.h"
#include "keypos/keypos.h"
#include <errno.h>
#include <inttypes.h>
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

/**
 * The prefix of each map value; clock_len clock entries (vclock_entry), then the value bytes,
 * follow it. deleted: unix ms, 0 if live.
 */
typedef struct {
  uint64_t ts;
  int64_t deleted;
  int32_t coord;
  uint32_t clock_len;
} entry_head;

/**
 * One queued put (deleted 0) or tombstone, in one allocation: key, '\0', the clock text, '\0',
 * then the value.
 */
typedef struct change {
  struct change *next;
  // Removes the row instead of writing it.
  bool remove;
  uint64_t ts;
  int coord;
  // Unix ms of the delete, 0 for a put.
  int64_t deleted;
  size_t value_len;
  char *clock;
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
  // Flush waiters not fired yet, and whether one wants a commit before the interval ends.
  // Guarded by mu.
  flush_waiter *waiters;
  bool flush_now;
  // Changes the writer took but did not commit yet, oldest first. Writer thread only.
  change *batch;
  change **batch_tail;
  size_t batch_len;
  // Changes the writer dropped. Writer thread only until it is joined.
  size_t dropped;
};

static change *change_new(const char *key, const void *value, size_t value_len, const vstamp *stamp,
                          int64_t deleted) {
  char clock[VCLOCK_TEXT_MAX];
  size_t clock_len = vclock_encode(&stamp->clock, clock);
  size_t key_len = strlen(key);
  change *c = malloc(sizeof(*c) + key_len + 1 + clock_len + 1 + value_len);
  if (!c) {
    return NULL;
  }
  c->next = NULL;
  c->remove = false;
  c->ts = stamp->ts;
  c->coord = stamp->coord;
  c->deleted = deleted;
  c->value_len = value_len;
  memcpy(c->key, key, key_len + 1);
  c->clock = c->key + key_len + 1;
  memcpy(c->clock, clock, clock_len + 1);
  c->value = c->clock + clock_len + 1;
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

/** Put stamp, deleted and value_len bytes of value under key in the map. Returns 0 or -1. */
static int map_put(hashmap *map, const char *key, const vstamp *stamp, int64_t deleted,
                   const void *value, size_t value_len) {
  entry_head head = {stamp->ts, deleted, stamp->coord, (uint32_t)stamp->clock.len};
  size_t clock_bytes = stamp->clock.len * sizeof(vclock_entry);
  unsigned char stack[256];
  size_t len = sizeof(head) + clock_bytes + value_len;
  unsigned char *buf = len <= sizeof(stack) ? stack : malloc(len);
  if (!buf) {
    return -1;
  }
  memcpy(buf, &head, sizeof(head));
  if (clock_bytes) {
    memcpy(buf + sizeof(head), stamp->clock.e, clock_bytes);
  }
  if (value_len) {
    memcpy(buf + sizeof(head) + clock_bytes, value, value_len);
  }
  int rc = hashmap_put(map, key, buf, len);
  if (buf != stack) {
    free(buf);
  }
  return rc;
}

/**
 * Decode the map entry raw of len bytes into *stamp and *deleted, and set *value_at to the offset
 * of its value. Returns 0, or -1 if malformed.
 */
static int decode_entry(const void *raw, size_t len, vstamp *stamp, int64_t *deleted,
                        size_t *value_at) {
  entry_head head;
  if (len < sizeof(head)) {
    return -1;
  }
  memcpy(&head, raw, sizeof(head));
  size_t clock_bytes = (size_t)head.clock_len * sizeof(vclock_entry);
  if (head.clock_len > VCLOCK_MAX || len < sizeof(head) + clock_bytes) {
    return -1;
  }
  stamp->ts = head.ts;
  stamp->coord = head.coord;
  stamp->clock.len = head.clock_len;
  if (clock_bytes) {
    memcpy(stamp->clock.e, (const unsigned char *)raw + sizeof(head), clock_bytes);
  }
  *deleted = head.deleted;
  *value_at = sizeof(head) + clock_bytes;
  return 0;
}

/**
 * Read the stamp and deleted of key's map entry into *stamp and *deleted. Returns 1 if found, 0 if
 * missing, -1 on error. If value is not NULL, set *value to a malloc buffer (NULL if empty) that
 * holds the value.
 */
static int map_get(hashmap *map, const char *key, vstamp *stamp, int64_t *deleted, void **value,
                   size_t *value_len) {
  void *raw = NULL;
  size_t len = 0;
  int rc = hashmap_get(map, key, &raw, &len);
  if (rc != 1) {
    return rc;
  }
  size_t at = 0;
  if (decode_entry(raw, len, stamp, deleted, &at) != 0) {
    free(raw);
    return -1;
  }
  if (value && len > at) {
    *value_len = len - at;
    memmove(raw, (unsigned char *)raw + at, *value_len);
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

/**
 * Decode the stamp in the columns ts, clock and coord of stmt, from column ts on. Returns 0, or -1
 * if the clock is malformed.
 */
static int column_stamp(sqlite3_stmt *stmt, int ts, vstamp *out) {
  out->ts = (uint64_t)sqlite3_column_int64(stmt, ts);
  out->coord = sqlite3_column_int(stmt, ts + 2);
  const char *clock = (const char *)sqlite3_column_text(stmt, ts + 1);
  return vclock_decode(clock ? clock : "", (size_t)sqlite3_column_bytes(stmt, ts + 1), &out->clock);
}

static int load_rows(kv_store *store) {
  sqlite3_stmt *select = NULL;
  if (sqlite3_prepare_v2(store->db, "SELECT key, value, deleted, ts, clock, coord FROM kv", -1,
                         &select, NULL) != SQLITE_OK) {
    return -1;
  }
  int rc;
  vstamp stamp;
  while ((rc = sqlite3_step(select)) == SQLITE_ROW) {
    const unsigned char *key = sqlite3_column_text(select, 0);
    const void *value = sqlite3_column_blob(select, 1);
    size_t value_len = (size_t)sqlite3_column_bytes(select, 1);
    if (!key || column_stamp(select, 3, &stamp) != 0 ||
        map_put(store->map, (const char *)key, &stamp, sqlite3_column_int64(select, 2), value,
                value_len) != 0) {
      break;
    }
  }
  sqlite3_finalize(select);
  return rc == SQLITE_DONE ? 0 : -1;
}

/** Return 1 if the kv table has the column name, 0 if not, -1 on error. */
static int has_column(sqlite3 *db, const char *name) {
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM pragma_table_info('kv') WHERE name = ?1", -1,
                         &stmt, NULL) != SQLITE_OK) {
    return -1;
  }
  (void)sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  int has = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) == 1 : -1;
  sqlite3_finalize(stmt);
  return has;
}

/**
 * Bring a kv table of an older node up to date: a table from before versions gets the ts and
 * deleted columns; the LWW version column becomes ts, under an empty clock; then the clock and
 * coord columns are added.
 */
static int upgrade_schema(sqlite3 *db) {
  int has_ts = has_column(db, "ts");
  int has_version = has_column(db, "version");
  int has_clock = has_column(db, "clock");
  if (has_ts < 0 || has_version < 0 || has_clock < 0) {
    return -1;
  }
  const char *ts = "";
  if (!has_ts) {
    ts = has_version ? "ALTER TABLE kv RENAME COLUMN version TO ts;"
                     : "ALTER TABLE kv ADD COLUMN ts INTEGER NOT NULL DEFAULT 0;"
                       "ALTER TABLE kv ADD COLUMN deleted INTEGER NOT NULL DEFAULT 0;";
  }
  const char *clock = has_clock ? ""
                                : "ALTER TABLE kv ADD COLUMN clock TEXT NOT NULL DEFAULT '';"
                                  "ALTER TABLE kv ADD COLUMN coord INTEGER NOT NULL DEFAULT 0;";
  char sql[512];
  (void)snprintf(sql, sizeof(sql), "BEGIN;%s%sCOMMIT;", ts, clock);
  return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
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
    rc = sqlite3_bind_int64(stmt, 3, (sqlite3_int64)c->ts);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(stmt, 4, c->deleted);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_text(stmt, 5, c->clock, -1, SQLITE_STATIC);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int(stmt, 6, c->coord);
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

/**
 * Call and free, outside mu, each waiter whose sequence number is at most seq. Call on the writer
 * thread, or after it is joined.
 */
static void fire_waiters(kv_store *store, uint64_t seq) {
  int status = store->dropped ? -1 : 0;
  pthread_mutex_lock(&store->mu);
  if (seq != UINT64_MAX && seq > store->committed_seq) {
    store->committed_seq = seq;
  }
  flush_waiter *due = NULL;
  for (flush_waiter **link = &store->waiters; *link;) {
    flush_waiter *w = *link;
    if (w->seq <= seq) {
      *link = w->next;
      w->next = due;
      due = w;
    } else {
      link = &w->next;
    }
  }
  pthread_mutex_unlock(&store->mu);
  while (due) {
    flush_waiter *next = due->next;
    due->fn(due->ctx, status);
    free(due);
    due = next;
  }
}

/**
 * Once per second, remove up to 1000 tombstones older than the TTL from the database and the map,
 * unless kv_store_pause_purge paused it. Writer thread only.
 */
static void purge(kv_store *store) {
  int64_t now = now_ms();
  if (now - store->last_purge_ms < 1000) {
    return;
  }
  store->last_purge_ms = now;
  pthread_mutex_lock(&store->purge_mu);
  if (store->purge_paused) {
    pthread_mutex_unlock(&store->purge_mu);
    return;
  }
  sqlite3_bind_int64(store->purge_select, 1, now - store->tombstone_ttl_ms);
  int rc;
  size_t purged = 0;
  while ((rc = sqlite3_step(store->purge_select)) == SQLITE_ROW) {
    const char *key = (const char *)sqlite3_column_text(store->purge_select, 0);
    int64_t stamp = sqlite3_column_int64(store->purge_select, 1);
    vstamp head;
    int64_t deleted = 0;
    pthread_mutex_lock(&store->mu);
    // A newer put or delete keeps its map entry; the database row then waits for its change.
    if (key && map_get(store->map, key, &head, &deleted, NULL, NULL) == 1 && deleted == stamp) {
      (void)hashmap_del(store->map, key);
    }
    pthread_mutex_unlock(&store->mu);
    sqlite3_bind_text(store->purge_delete, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(store->purge_delete, 2, stamp);
    if (sqlite3_step(store->purge_delete) == SQLITE_DONE) {
      purged++;
    }
    sqlite3_reset(store->purge_delete);
  }
  if (rc != SQLITE_DONE) {
    store->log.log(store->log.ctx, LOG_ERROR, "kv_store: purge failed: %s",
                   sqlite3_errmsg(store->db));
  }
  sqlite3_reset(store->purge_select);
  pthread_mutex_unlock(&store->purge_mu);
  if (purged) {
    store->log.log(store->log.ctx, LOG_INFO, "kv_store: purged %zu tombstones", purged);
  }
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
    while (!store->stop && !store->flush_now && store->queued < KV_STORE_WAKE_LEN &&
           pthread_cond_timedwait(&store->wake, &store->mu, &deadline) != ETIMEDOUT) {
    }
    bool stopping = store->stop;
    store->flush_now = false;
    uint64_t taken_seq = store->last_seq;
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
    if (!store->batch) {
      fire_waiters(store, taken_seq);
      purge(store);
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

static void close_db(kv_store *store) {
  sqlite3_finalize(store->upsert);
  sqlite3_finalize(store->remove);
  sqlite3_finalize(store->purge_select);
  sqlite3_finalize(store->purge_delete);
  sqlite3_finalize(store->dump);
  sqlite3_close(store->reader);
  sqlite3_close(store->db);
}

kv_store *kv_store_new(const kv_store_options *opt, logger log) {
  const char *db_path = opt->db_path;
  kv_store *store = calloc(1, sizeof(*store));
  if (!store) {
    return NULL;
  }
  store->log = log;
  store->flush_interval_ms = opt->flush_interval_ms;
  store->max_pending = opt->max_pending;
  store->tombstone_ttl_ms = (int64_t)opt->tombstone_ttl_s * 1000;
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
  pthread_mutex_init(&store->reader_mu, NULL);
  pthread_mutex_init(&store->purge_mu, NULL);

  store->map = hashmap_new();
  if (!store->map) {
    goto fail;
  }
  if (sqlite3_open(db_path, &store->db) != SQLITE_OK ||
      sqlite3_exec(store->db,
                   "PRAGMA journal_mode=WAL;"
                   "PRAGMA synchronous=FULL;"
                   "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value BLOB,"
                   " ts INTEGER NOT NULL DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0,"
                   " clock TEXT NOT NULL DEFAULT '', coord INTEGER NOT NULL DEFAULT 0)"
                   " WITHOUT ROWID;",
                   NULL, NULL, NULL) != SQLITE_OK ||
      upgrade_schema(store->db) != 0 ||
      sqlite3_prepare_v2(store->db,
                         "INSERT INTO kv(key, value, ts, deleted, clock, coord)"
                         " VALUES(?1, ?2, ?3, ?4, ?5, ?6)"
                         " ON CONFLICT(key) DO UPDATE SET value = excluded.value,"
                         " ts = excluded.ts, deleted = excluded.deleted,"
                         " clock = excluded.clock, coord = excluded.coord",
                         -1, &store->upsert, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(store->db, "DELETE FROM kv WHERE key = ?1", -1, &store->remove, NULL) !=
          SQLITE_OK ||
      sqlite3_prepare_v2(
          store->db, "SELECT key, deleted FROM kv WHERE deleted != 0 AND deleted < ?1 LIMIT 1000",
          -1, &store->purge_select, NULL) != SQLITE_OK ||
      sqlite3_prepare_v2(store->db, "DELETE FROM kv WHERE key = ?1 AND deleted = ?2", -1,
                         &store->purge_delete, NULL) != SQLITE_OK ||
      sqlite3_open_v2(db_path, &store->reader, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
      sqlite3_busy_timeout(store->reader, 1000) != SQLITE_OK ||
      sqlite3_prepare_v2(store->reader,
                         "SELECT key, value, deleted, ts, clock, coord FROM kv WHERE key > ?1"
                         " ORDER BY key LIMIT ?2",
                         -1, &store->dump, NULL) != SQLITE_OK ||
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
  close_db(store);
  hashmap_free(store->map);
  pthread_mutex_destroy(&store->reader_mu);
  pthread_mutex_destroy(&store->purge_mu);
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
  fire_waiters(store, UINT64_MAX);
  change_free_all(store->head);
  change_free_all(store->batch);
  close_db(store);
  hashmap_free(store->map);
  pthread_mutex_destroy(&store->reader_mu);
  pthread_mutex_destroy(&store->purge_mu);
  pthread_mutex_destroy(&store->mu);
  pthread_cond_destroy(&store->wake);
  free(store);
  return status;
}

int kv_store_get(kv_store *store, const char *key, void **out_value, size_t *out_len) {
  kv_record r;
  int rc = kv_store_read(store, key, &r);
  if (rc != 1 || r.deleted) {
    free(r.value);
    *out_value = NULL;
    *out_len = 0;
    return rc == 1 ? 0 : rc;
  }
  *out_value = r.value;
  *out_len = r.value_len;
  return 1;
}

int kv_store_read(kv_store *store, const char *key, kv_record *out) {
  int64_t deleted = 0;
  out->value = NULL;
  out->value_len = 0;
  int rc = map_get(store->map, key, &out->stamp, &deleted, &out->value, &out->value_len);
  out->deleted = deleted != 0;
  return rc;
}

/**
 * Put the entry under key in the map and queue it. Call with mu held. Returns 0, or -1 (the map
 * may be changed).
 */
static int store_entry(kv_store *store, const char *key, const vstamp *stamp, int64_t deleted,
                       const void *value, size_t value_len) {
  change *c = change_new(key, value, value_len, stamp, deleted);
  if (!c || map_put(store->map, key, stamp, deleted, value, value_len) != 0) {
    free(c);
    return -1;
  }
  enqueue(store, c);
  return 0;
}

typedef enum {
  /** max_pending changes wait; the map is unchanged. */
  APPLY_FULL = -2,
  APPLY_ERROR = -1,
  /** The change won; no live value was there. */
  APPLY_NEW = 0,
  /** The change won over a live value. */
  APPLY_REPLACED = 1,
  /** The stored entry won. */
  APPLY_LOST = 2,
} apply_result;

/**
 * Apply a put (deleted 0) or tombstone with stamp (NULL: empty) to the map by the winner rule and
 * queue the entry. Map and queue change under one lock so the queue holds changes in map order,
 * and the database ends with the entry the map holds.
 */
static apply_result apply(kv_store *store, const char *key, const void *value, size_t value_len,
                          const vstamp *stamp, int64_t deleted) {
  static const vstamp none = {0};
  stamp = stamp ? stamp : &none;
  vstamp old;
  vstamp winner;
  int64_t old_deleted = 0;
  void *old_value = NULL;
  size_t old_len = 0;
  pthread_mutex_lock(&store->mu);
  apply_result status = APPLY_FULL;
  if (store->queued + store->in_batch < store->max_pending) {
    int found = map_get(store->map, key, &old, &old_deleted, &old_value, &old_len);
    int pick = found == 1 ? vstamp_pick(stamp, &old, &winner) : 0;
    if (found < 0) {
      status = APPLY_ERROR;
    } else if (pick == 0) {
      status = found && !old_deleted ? APPLY_REPLACED : APPLY_NEW;
      if (store_entry(store, key, found == 1 ? &winner : stamp, deleted, value, value_len) != 0) {
        status = APPLY_ERROR;
      }
    } else {
      status = APPLY_LOST;
      // A concurrent change lost, but the stored clock takes in its clock.
      if (vclock_compare(&winner.clock, &old.clock) != VCLOCK_EQUAL &&
          store_entry(store, key, &winner, old_deleted, old_value, old_len) != 0) {
        status = APPLY_ERROR;
      }
    }
  }
  pthread_mutex_unlock(&store->mu);
  free(old_value);
  return status;
}

int kv_store_put(kv_store *store, const char *key, const void *value, size_t value_len,
                 const vstamp *stamp) {
  apply_result rc = apply(store, key, value, value_len, stamp, 0);
  return rc == APPLY_FULL || rc == APPLY_ERROR ? rc : 0;
}

int kv_store_del(kv_store *store, const char *key, const vstamp *stamp) {
  apply_result rc = apply(store, key, NULL, 0, stamp, now_ms());
  return rc == APPLY_LOST ? APPLY_REPLACED : rc;
}

int kv_store_remove(kv_store *store, const char *key) {
  static const vstamp none = {0};
  change *c = change_new(key, NULL, 0, &none, 0);
  if (!c) {
    return -1;
  }
  c->remove = true;
  pthread_mutex_lock(&store->mu);
  int status =
      store->queued + store->in_batch >= store->max_pending ? -2 : hashmap_del(store->map, key);
  if (status == 1) {
    enqueue(store, c);
  }
  pthread_mutex_unlock(&store->mu);
  if (status != 1) {
    free(c);
  }
  return status;
}

int kv_store_dump(kv_store *store, const char *after, size_t limit, kv_dump_fn fn, void *ctx) {
  pthread_mutex_lock(&store->reader_mu);
  sqlite3_stmt *stmt = store->dump;
  int rc = sqlite3_bind_text(stmt, 1, after, -1, SQLITE_STATIC);
  if (rc == SQLITE_OK) {
    rc = sqlite3_bind_int64(stmt, 2, (sqlite3_int64)limit);
  }
  int count = 0;
  while (rc == SQLITE_OK && (rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    rc = SQLITE_OK;
    kv_dump_row row = {
        .key = (const char *)sqlite3_column_text(stmt, 0),
        .value = sqlite3_column_blob(stmt, 1),
        .value_len = (size_t)sqlite3_column_bytes(stmt, 1),
        .deleted = sqlite3_column_int64(stmt, 2) != 0,
    };
    if (!row.key || column_stamp(stmt, 3, &row.stamp) != 0 || fn(ctx, &row) != 0) {
      rc = SQLITE_ERROR;
      break;
    }
    count++;
  }
  sqlite3_reset(stmt);
  sqlite3_clear_bindings(stmt);
  pthread_mutex_unlock(&store->reader_mu);
  return rc == SQLITE_DONE ? count : -1;
}

typedef struct {
  const uint64_t *ends;
  size_t n;
  uint64_t *hashes;
} digest_ctx;

static int digest_entry(const char *key, size_t key_len, const void *val, size_t val_len,
                        void *arg) {
  digest_ctx *d = arg;
  vstamp stamp;
  int64_t deleted = 0;
  size_t at = 0;
  if (decode_entry(val, val_len, &stamp, &deleted, &at) != 0) {
    return -1;
  }
  d->hashes[key_segment(key_position(key, key_len), d->ends, d->n)] ^=
      key_entry_hash(key, key_len, &stamp, deleted != 0);
  return 0;
}

int kv_store_digest(kv_store *store, const uint64_t *ends, size_t n, uint64_t *hashes) {
  memset(hashes, 0, n * sizeof(*hashes));
  digest_ctx d = {ends, n, hashes};
  return n == 0 || hashmap_foreach(store->map, digest_entry, &d) == 0 ? 0 : -1;
}

typedef struct {
  uint64_t start;
  uint64_t end;
  kv_dump_fn fn;
  void *ctx;
  int count;
} range_ctx;

static int range_entry(const char *key, size_t key_len, const void *val, size_t val_len,
                       void *arg) {
  range_ctx *r = arg;
  if (!key_in_segment(key_position(key, key_len), r->start, r->end)) {
    return 0;
  }
  kv_dump_row row = {.key = key};
  int64_t deleted = 0;
  size_t at = 0;
  if (decode_entry(val, val_len, &row.stamp, &deleted, &at) != 0) {
    return -1;
  }
  row.value = (const unsigned char *)val + at;
  row.value_len = val_len - at;
  row.deleted = deleted != 0;
  r->count++;
  return r->fn(r->ctx, &row) == 0 ? 0 : -1;
}

int kv_store_range(kv_store *store, uint64_t start, uint64_t end, kv_dump_fn fn, void *ctx) {
  range_ctx r = {start, end, fn, ctx, 0};
  return hashmap_foreach(store->map, range_entry, &r) == 0 ? r.count : -1;
}

void kv_store_pause_purge(kv_store *store, bool paused) {
  pthread_mutex_lock(&store->purge_mu);
  store->purge_paused = paused;
  pthread_mutex_unlock(&store->purge_mu);
}

int kv_store_flush(kv_store *store, bool now, kv_flush_fn fn, void *ctx) {
  flush_waiter *w = malloc(sizeof(*w));
  if (!w) {
    return -1;
  }
  pthread_mutex_lock(&store->mu);
  if (store->stop) {
    pthread_mutex_unlock(&store->mu);
    free(w);
    return -1;
  }
  bool done = store->last_seq <= store->committed_seq;
  if (!done) {
    *w = (flush_waiter){store->waiters, store->last_seq, fn, ctx};
    store->waiters = w;
    if (now) {
      store->flush_now = true;
      pthread_cond_signal(&store->wake);
    }
  }
  pthread_mutex_unlock(&store->mu);
  if (done) {
    free(w);
    fn(ctx, 0);
  }
  return 0;
}
