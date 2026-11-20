#include "kv/kv_store.h"
#include "vclock/vclock.h"

#include <assert.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static atomic_int errors;

static void count_errors(void *ctx, log_level level, const char *fmt, ...) {
  (void)ctx;
  if (level == LOG_ERROR) {
    atomic_fetch_add(&errors, 1);
  }
  va_list ap;
  va_start(ap, fmt);
  (void)vfprintf(stderr, fmt, ap);
  va_end(ap);
  (void)fputc('\n', stderr);
}

static const logger test_log = {count_errors, NULL};

static kv_store *open_store(const char *db, int flush_interval_ms, size_t max_pending) {
  kv_store_options opt = {db, flush_interval_ms, max_pending, 3600};
  return kv_store_new(&opt, test_log);
}

static void expect_value(kv_store *store, const char *key, const void *want, size_t want_len) {
  void *got = NULL;
  size_t got_len = 0;
  assert(kv_store_get(store, key, &got, &got_len) == 1);
  assert(got_len == want_len);
  assert(want_len == 0 || memcmp(got, want, want_len) == 0);
  free(got);
}

static void test_values_survive_restart(const char *db) {
  static const char multiline[] = "line one\nline two\n";
  static const char binary[] = {'\0', '\x01', '\xff', '\n', '\r', ' ', '\0'};

  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  assert(kv_store_put(store, "multi", multiline, sizeof(multiline) - 1, NULL) == 0);
  assert(kv_store_put(store, "bin", binary, sizeof(binary), NULL) == 0);
  assert(kv_store_put(store, "empty", NULL, 0, NULL) == 0);
  assert(kv_store_put(store, "key with space", "v", 1, NULL) == 0);
  assert(kv_store_put(store, "over", "old", 3, NULL) == 0);
  assert(kv_store_put(store, "over", "new", 3, NULL) == 0);
  assert(kv_store_put(store, "gone", "x", 1, NULL) == 0);
  assert(kv_store_del(store, "gone", NULL) == 1);
  assert(kv_store_free(store) == 0);

  store = open_store(db, 10, 1000000);
  assert(store);
  expect_value(store, "multi", multiline, sizeof(multiline) - 1);
  expect_value(store, "bin", binary, sizeof(binary));
  expect_value(store, "empty", NULL, 0);
  expect_value(store, "key with space", "v", 1);
  expect_value(store, "over", "new", 3);
  void *got = NULL;
  size_t got_len = 0;
  assert(kv_store_get(store, "gone", &got, &got_len) == 0);
  assert(kv_store_free(store) == 0);
  assert(atomic_load(&errors) == 0);
  printf("  test_values_survive_restart OK\n");
}

static int count_rows(sqlite3 *db, const char *key) {
  sqlite3_stmt *stmt = NULL;
  assert(sqlite3_prepare_v2(db, "SELECT count(*) FROM kv WHERE key = ?", -1, &stmt, NULL) ==
         SQLITE_OK);
  assert(sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  int n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static vstamp stamp(const char *clock, uint64_t ts, int coord) {
  vstamp s = {.ts = ts, .coord = coord};
  assert(vclock_decode(clock, strlen(clock), &s.clock) == 0);
  return s;
}

/** Assert key holds want (NULL: a tombstone) with clock, ts and coord. */
static void expect_record(kv_store *store, const char *key, const char *want, const char *clock,
                          uint64_t ts, int coord) {
  kv_record r;
  assert(kv_store_read(store, key, &r) == 1);
  assert(r.deleted == (want == NULL));
  assert(r.value_len == (want ? strlen(want) : 0));
  assert(!want || memcmp(r.value, want, r.value_len) == 0);
  char text[VCLOCK_TEXT_MAX];
  (void)vclock_encode(&r.stamp.clock, text);
  assert(strcmp(text, clock) == 0);
  assert(r.stamp.ts == ts && r.stamp.coord == coord);
  free(r.value);
}

static void test_put_with_a_dominating_clock_replaces_and_a_dominated_one_does_not(const char *db) {
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  vstamp first = stamp("1:1", 100, 1);
  vstamp second = stamp("1:2", 90, 1);
  assert(kv_store_put(store, "d", "first", 5, &first) == 0);
  assert(kv_store_put(store, "d", "second", 6, &second) == 0);
  expect_record(store, "d", "second", "1:2", 90, 1);
  assert(kv_store_put(store, "d", "first", 5, &first) == 0);
  expect_record(store, "d", "second", "1:2", 90, 1);
  assert(kv_store_free(store) == 0);
  printf("  test_put_with_a_dominating_clock_replaces_and_a_dominated_one_does_not OK\n");
}

static void test_concurrent_puts_keep_the_winner_with_the_merged_clock(const char *db) {
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  vstamp a = stamp("1:1", 100, 1);
  vstamp b = stamp("2:1", 50, 2);
  vstamp c = stamp("3:1", 200, 3);
  assert(kv_store_put(store, "c", "a", 1, &a) == 0);
  assert(kv_store_put(store, "c", "b", 1, &b) == 0);
  expect_record(store, "c", "a", "1:1,2:1", 100, 1);
  assert(kv_store_put(store, "c", "c", 1, &c) == 0);
  expect_record(store, "c", "c", "1:1,2:1,3:1", 200, 3);
  assert(kv_store_free(store) == 0);
  store = open_store(db, 10, 1000000);
  assert(store);
  expect_record(store, "c", "c", "1:1,2:1,3:1", 200, 3);
  assert(kv_store_free(store) == 0);
  printf("  test_concurrent_puts_keep_the_winner_with_the_merged_clock OK\n");
}

static void test_tombstone_carries_its_clock(const char *db) {
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  vstamp put = stamp("1:2", 10, 1);
  vstamp stale_del = stamp("1:1", 20, 1);
  vstamp del = stamp("1:3", 30, 1);
  assert(kv_store_put(store, "t", "live", 4, &put) == 0);
  assert(kv_store_del(store, "t", &stale_del) == 1);
  expect_value(store, "t", "live", 4);
  assert(kv_store_del(store, "t", &del) == 1);
  expect_record(store, "t", NULL, "1:3", 30, 1);
  assert(kv_store_put(store, "t", "live", 4, &put) == 0);
  assert(kv_store_free(store) == 0);
  store = open_store(db, 10, 1000000);
  assert(store);
  expect_record(store, "t", NULL, "1:3", 30, 1);
  assert(kv_store_free(store) == 0);
  printf("  test_tombstone_carries_its_clock OK\n");
}

static void test_version_column_becomes_the_timestamp_of_an_empty_clock(const char *db) {
  sqlite3 *old = NULL;
  assert(sqlite3_open(db, &old) == SQLITE_OK);
  assert(sqlite3_exec(old,
                      "CREATE TABLE kv(key TEXT PRIMARY KEY, value BLOB, version INTEGER NOT NULL"
                      " DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;"
                      "INSERT INTO kv VALUES('v', CAST('lww' AS BLOB), 42, 0);",
                      NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(old);
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  expect_record(store, "v", "lww", "", 42, 0);
  vstamp older = stamp("", 41, 0);
  vstamp clocked = stamp("1:1", 1, 1);
  assert(kv_store_put(store, "v", "older", 5, &older) == 0);
  expect_record(store, "v", "lww", "", 42, 0);
  assert(kv_store_put(store, "v", "clocked", 7, &clocked) == 0);
  assert(kv_store_free(store) == 0);
  store = open_store(db, 10, 1000000);
  assert(store);
  expect_record(store, "v", "clocked", "1:1", 1, 1);
  assert(kv_store_free(store) == 0);
  printf("  test_version_column_becomes_the_timestamp_of_an_empty_clock OK\n");
}

static void test_failed_commit_keeps_changes(const char *db) {
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);

  sqlite3 *locker = NULL;
  assert(sqlite3_open(db, &locker) == SQLITE_OK);
  assert(sqlite3_exec(locker, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);

  atomic_store(&errors, 0);
  assert(kv_store_put(store, "late", "kept\n", 5, NULL) == 0);
  struct timespec tick = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int i = 0; i != 500 && atomic_load(&errors) < 2; i++) {
    nanosleep(&tick, NULL);
  }
  assert(atomic_load(&errors) >= 2);
  assert(count_rows(locker, "late") == 0);

  assert(sqlite3_exec(locker, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(locker);
  assert(kv_store_free(store) == 0);

  store = open_store(db, 10, 1000000);
  assert(store);
  expect_value(store, "late", "kept\n", 5);
  assert(kv_store_free(store) == 0);
  printf("  test_failed_commit_keeps_changes OK\n");
}

static void test_locked_shutdown_is_quick_and_fails(const char *db) {
  kv_store *store = open_store(db, 3600000, 1000000);
  assert(store);
  sqlite3 *locker = NULL;
  assert(sqlite3_open(db, &locker) == SQLITE_OK);
  assert(sqlite3_exec(locker, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
  assert(kv_store_put(store, "lost", "x", 1, NULL) == 0);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  assert(kv_store_free(store) == -1);
  clock_gettime(CLOCK_MONOTONIC, &end);
  assert(end.tv_sec - start.tv_sec < 5);

  assert(sqlite3_exec(locker, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(locker);
  printf("  test_locked_shutdown_is_quick_and_fails OK\n");
}

static void test_dropped_table_keeps_changes(const char *db) {
  kv_store *store = open_store(db, 10, 1000000);
  assert(store);
  sqlite3 *other = NULL;
  assert(sqlite3_open(db, &other) == SQLITE_OK);
  assert(sqlite3_busy_timeout(other, 5000) == SQLITE_OK);
  assert(sqlite3_exec(other, "DROP TABLE kv", NULL, NULL, NULL) == SQLITE_OK);

  atomic_store(&errors, 0);
  assert(kv_store_put(store, "a", "1", 1, NULL) == 0);
  assert(kv_store_put(store, "b", "2", 1, NULL) == 0);
  struct timespec tick = {.tv_sec = 0, .tv_nsec = 10000000};
  for (int i = 0; i != 500 && atomic_load(&errors) < 2; i++) {
    nanosleep(&tick, NULL);
  }
  assert(atomic_load(&errors) >= 2);

  assert(sqlite3_exec(other,
                      "CREATE TABLE kv(key TEXT PRIMARY KEY, value BLOB, ts INTEGER NOT NULL"
                      " DEFAULT 0, deleted INTEGER NOT NULL DEFAULT 0, clock TEXT NOT NULL"
                      " DEFAULT '', coord INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID",
                      NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(other);
  assert(kv_store_free(store) == 0);

  store = open_store(db, 10, 1000000);
  assert(store);
  expect_value(store, "a", "1", 1);
  expect_value(store, "b", "2", 1);
  assert(kv_store_free(store) == 0);
  printf("  test_dropped_table_keeps_changes OK\n");
}

static void test_pending_limit(const char *db) {
  kv_store *store = open_store(db, 3600000, 2);
  assert(store);
  assert(kv_store_put(store, "p1", "1", 1, NULL) == 0);
  assert(kv_store_put(store, "p2", "2", 1, NULL) == 0);
  assert(kv_store_put(store, "p3", "3", 1, NULL) == -2);
  assert(kv_store_del(store, "p1", NULL) == -2);
  void *got = NULL;
  size_t got_len = 0;
  assert(kv_store_get(store, "p3", &got, &got_len) == 0);
  expect_value(store, "p1", "1", 1);
  assert(kv_store_free(store) == 0);

  store = open_store(db, 10, 2);
  assert(store);
  assert(kv_store_put(store, "p3", "3", 1, NULL) == 0);
  assert(kv_store_put(store, "p4", "4", 1, NULL) == 0);
  struct timespec tick = {.tv_sec = 0, .tv_nsec = 10000000};
  int rc = -2;
  for (int i = 0; i != 500 && rc == -2; i++) {
    nanosleep(&tick, NULL);
    rc = kv_store_put(store, "p5", "5", 1, 0);
  }
  assert(rc == 0);
  assert(kv_store_free(store) == 0);
  printf("  test_pending_limit OK\n");
}

static void test_failing_change_does_not_block_others(const char *db) {
  sqlite3 *setup = NULL;
  assert(sqlite3_open(db, &setup) == SQLITE_OK);
  assert(
      sqlite3_exec(setup,
                   "CREATE TABLE IF NOT EXISTS kv(key TEXT PRIMARY KEY, value BLOB) WITHOUT ROWID;"
                   "CREATE TRIGGER reject_bad BEFORE INSERT ON kv WHEN NEW.key = 'bad'"
                   " BEGIN SELECT RAISE(ABORT, 'bad key'); END;",
                   NULL, NULL, NULL) == SQLITE_OK);
  sqlite3_close(setup);

  kv_store *store = open_store(db, 3600000, 1000000);
  assert(store);
  atomic_store(&errors, 0);
  assert(kv_store_put(store, "before", "1", 1, NULL) == 0);
  assert(kv_store_put(store, "bad", "2", 1, NULL) == 0);
  assert(kv_store_put(store, "after", "3", 1, NULL) == 0);
  assert(kv_store_free(store) == -1);
  assert(atomic_load(&errors) >= 1);

  store = open_store(db, 3600000, 1000000);
  assert(store);
  expect_value(store, "before", "1", 1);
  expect_value(store, "after", "3", 1);
  void *got = NULL;
  size_t got_len = 0;
  assert(kv_store_get(store, "bad", &got, &got_len) == 0);
  assert(kv_store_free(store) == 0);
  printf("  test_failing_change_does_not_block_others OK\n");
}

int main(void) {
  char dir[] = "/tmp/kv_store_test.XXXXXX";
  assert(mkdtemp(dir));
  char db[sizeof(dir) + 16];
  (void)snprintf(db, sizeof(db), "%s/kv.db", dir);

  printf("kv_store tests:\n");
  test_values_survive_restart(db);
  test_put_with_a_dominating_clock_replaces_and_a_dominated_one_does_not(db);
  test_failed_commit_keeps_changes(db);
  test_locked_shutdown_is_quick_and_fails(db);
  test_dropped_table_keeps_changes(db);
  test_pending_limit(db);
  char bad_db[sizeof(dir) + 16];
  (void)snprintf(bad_db, sizeof(bad_db), "%s/bad.db", dir);
  test_failing_change_does_not_block_others(bad_db);
  test_concurrent_puts_keep_the_winner_with_the_merged_clock(db);
  test_tombstone_carries_its_clock(db);
  char old_db[sizeof(dir) + 16];
  (void)snprintf(old_db, sizeof(old_db), "%s/old.db", dir);
  test_version_column_becomes_the_timestamp_of_an_empty_clock(old_db);
  printf("all kv_store tests passed\n");

  char path[sizeof(db) + 8];
  static const char *const suffixes[] = {"", "-wal", "-shm"};
  const char *const dbs[] = {db, bad_db, old_db};
  for (size_t d = 0; d != sizeof(dbs) / sizeof(dbs[0]); d++) {
    for (size_t i = 0; i != sizeof(suffixes) / sizeof(suffixes[0]); i++) {
      (void)snprintf(path, sizeof(path), "%s%s", dbs[d], suffixes[i]);
      (void)unlink(path);
    }
  }
  (void)rmdir(dir);
  return 0;
}
