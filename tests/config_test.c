#include "config/config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const VARS[] = {
    "KV_LISTEN_ADDR",   "KV_LOG_PATH", "KV_DB_PATH",
    "KV_SNAPSHOT_PATH", "KV_THREADS",  "KV_FLUSH_INTERVAL_MS",
};

static void set_required(void) {
  for (size_t i = 0; i != sizeof(VARS) / sizeof(VARS[0]); i++) {
    unsetenv(VARS[i]);
  }
  setenv("KV_LISTEN_ADDR", "127.0.0.1:7890", 1);
  setenv("KV_LOG_PATH", "kv.log", 1);
  setenv("KV_DB_PATH", "kv.db", 1);
}

static void test_required_set_uses_optional_defaults(void) {
  set_required();
  kv_config cfg;
  assert(config_load(&cfg) == 0);
  assert(strcmp(cfg.listen_addr, "127.0.0.1:7890") == 0);
  assert(strcmp(cfg.log_path, "kv.log") == 0);
  assert(strcmp(cfg.db_path, "kv.db") == 0);
  assert(strcmp(cfg.snapshot_path, "kv_snapshot_persistence.txt") == 0);
  assert(cfg.flush_interval_ms == 100);
  assert(cfg.threads == 0);
  printf("  test_required_set_uses_optional_defaults OK\n");
}

static void test_overrides(void) {
  set_required();
  setenv("KV_LISTEN_ADDR", "0.0.0.0:8080", 1);
  setenv("KV_LOG_PATH", "/tmp/a.log", 1);
  setenv("KV_DB_PATH", "/tmp/a.db", 1);
  setenv("KV_SNAPSHOT_PATH", "/tmp/a.txt", 1);
  setenv("KV_FLUSH_INTERVAL_MS", "250", 1);
  setenv("KV_THREADS", "4", 1);
  kv_config cfg;
  assert(config_load(&cfg) == 0);
  assert(strcmp(cfg.listen_addr, "0.0.0.0:8080") == 0);
  assert(strcmp(cfg.log_path, "/tmp/a.log") == 0);
  assert(strcmp(cfg.db_path, "/tmp/a.db") == 0);
  assert(strcmp(cfg.snapshot_path, "/tmp/a.txt") == 0);
  assert(cfg.flush_interval_ms == 250);
  assert(cfg.threads == 4);
  printf("  test_overrides OK\n");
}

static void test_empty_optional_uses_default(void) {
  set_required();
  setenv("KV_SNAPSHOT_PATH", "", 1);
  setenv("KV_THREADS", "", 1);
  setenv("KV_FLUSH_INTERVAL_MS", "", 1);
  kv_config cfg;
  assert(config_load(&cfg) == 0);
  assert(strcmp(cfg.snapshot_path, "kv_snapshot_persistence.txt") == 0);
  assert(cfg.threads == 0);
  assert(cfg.flush_interval_ms == 100);
  printf("  test_empty_optional_uses_default OK\n");
}

static void test_missing_required(void) {
  const char *required[] = {"KV_LISTEN_ADDR", "KV_LOG_PATH", "KV_DB_PATH"};
  kv_config cfg;
  for (size_t i = 0; i != 3; i++) {
    set_required();
    unsetenv(required[i]);
    assert(config_load(&cfg) == -1);
    set_required();
    setenv(required[i], "", 1);
    assert(config_load(&cfg) == -1);
  }
  set_required();
  unsetenv("KV_LISTEN_ADDR");
  unsetenv("KV_LOG_PATH");
  unsetenv("KV_DB_PATH");
  assert(config_load(&cfg) == -1);
  printf("  test_missing_required OK\n");
}

static void expect_bad(const char *name, const char *value) {
  set_required();
  setenv(name, value, 1);
  kv_config cfg;
  assert(config_load(&cfg) == -1);
}

static void test_bad_numbers(void) {
  expect_bad("KV_THREADS", "abc");
  expect_bad("KV_THREADS", "4x");
  expect_bad("KV_THREADS", "-1");
  expect_bad("KV_THREADS", "1025");
  expect_bad("KV_THREADS", "99999999999999999999");
  expect_bad("KV_FLUSH_INTERVAL_MS", "0");
  expect_bad("KV_FLUSH_INTERVAL_MS", "3600001");
  expect_bad("KV_FLUSH_INTERVAL_MS", " ");
  printf("  test_bad_numbers OK\n");
}

static void test_bad_listen_addr(void) {
  expect_bad("KV_LISTEN_ADDR", "localhost");
  printf("  test_bad_listen_addr OK\n");
}

int main(void) {
  printf("config tests:\n");
  test_required_set_uses_optional_defaults();
  test_overrides();
  test_empty_optional_uses_default();
  test_missing_required();
  test_bad_numbers();
  test_bad_listen_addr();
  printf("all config tests passed\n");
  return 0;
}
