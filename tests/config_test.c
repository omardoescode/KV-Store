#include "config/config.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const VARS[] = {
    "KV_LISTEN_ADDR",       "KV_LOG_PATH",    "KV_DB_PATH",  "KV_THREADS",
    "KV_FLUSH_INTERVAL_MS", "KV_MAX_PENDING", "KV_SYNC_ACK", "KV_TOMBSTONE_TTL_S",
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
  assert(cfg.flush_interval_ms == 100);
  assert(cfg.threads == 0);
  assert(cfg.max_pending == 1000000);
  assert(cfg.sync_ack == 0);
  assert(cfg.tombstone_ttl_s == 3600);
  printf("  test_required_set_uses_optional_defaults OK\n");
}

static void test_overrides(void) {
  set_required();
  setenv("KV_LISTEN_ADDR", "0.0.0.0:8080", 1);
  setenv("KV_LOG_PATH", "/tmp/a.log", 1);
  setenv("KV_DB_PATH", "/tmp/a.db", 1);
  setenv("KV_FLUSH_INTERVAL_MS", "250", 1);
  setenv("KV_THREADS", "4", 1);
  setenv("KV_MAX_PENDING", "5", 1);
  setenv("KV_SYNC_ACK", "1", 1);
  setenv("KV_TOMBSTONE_TTL_S", "60", 1);
  kv_config cfg;
  assert(config_load(&cfg) == 0);
  assert(strcmp(cfg.listen_addr, "0.0.0.0:8080") == 0);
  assert(strcmp(cfg.log_path, "/tmp/a.log") == 0);
  assert(strcmp(cfg.db_path, "/tmp/a.db") == 0);
  assert(cfg.flush_interval_ms == 250);
  assert(cfg.threads == 4);
  assert(cfg.max_pending == 5);
  assert(cfg.sync_ack == 1);
  assert(cfg.tombstone_ttl_s == 60);
  printf("  test_overrides OK\n");
}

static void test_empty_optional_uses_default(void) {
  set_required();
  setenv("KV_THREADS", "", 1);
  setenv("KV_FLUSH_INTERVAL_MS", "", 1);
  kv_config cfg;
  assert(config_load(&cfg) == 0);
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
  expect_bad("KV_MAX_PENDING", "0");
  expect_bad("KV_MAX_PENDING", "1000000001");
  expect_bad("KV_SYNC_ACK", "2");
  expect_bad("KV_SYNC_ACK", "-1");
  expect_bad("KV_TOMBSTONE_TTL_S", "0");
  printf("  test_bad_numbers OK\n");
}

static void test_bad_listen_addr(void) {
  expect_bad("KV_LISTEN_ADDR", "localhost");
  printf("  test_bad_listen_addr OK\n");
}

static const char *const CP_VARS[] = {
    "KV_CP_LISTEN_ADDR",      "KV_CP_DB_PATH",
    "KV_CP_LOG_PATH",         "KV_CP_THREADS",
    "KV_CP_INITIAL_NODES",    "KV_CP_MAX_NODES",
    "KV_CP_REPLICAS",         "KV_CP_VNODES",
    "KV_CP_SCALE_RPS",        "KV_CP_SCALE_WINDOW_S",
    "KV_CP_SCALE_COOLDOWN_S", "KV_CP_PROBE_INTERVAL_MS",
    "KV_CP_PROBE_FAILS",      "KV_CP_FORWARD_TIMEOUT_MS",
    "KV_CP_NODE_IMAGE",       "KV_CP_NODE_BASE_PORT",
    "KV_CP_DOCKER_SOCK",      "KV_CP_NODE_NETWORK",
    "KV_CP_NODE_BIN",         "KV_CP_NODE_DIR",
    "KV_CP_READ_QUORUM",      "KV_CP_WRITE_QUORUM",
};

static void cp_set_required(void) {
  for (size_t i = 0; i != sizeof(CP_VARS) / sizeof(CP_VARS[0]); i++) {
    unsetenv(CP_VARS[i]);
  }
  setenv("KV_CP_LISTEN_ADDR", "0.0.0.0:7890", 1);
  setenv("KV_CP_DB_PATH", "cp.db", 1);
  setenv("KV_CP_LOG_PATH", "-", 1);
}

static void test_cp_defaults(void) {
  cp_set_required();
  cp_config cfg;
  assert(cp_config_load(&cfg) == 0);
  assert(strcmp(cfg.listen_addr, "0.0.0.0:7890") == 0);
  assert(strcmp(cfg.db_path, "cp.db") == 0);
  assert(strcmp(cfg.log_path, "-") == 0);
  assert(cfg.threads == 0);
  assert(cfg.initial_nodes == 3);
  assert(cfg.max_nodes == 3);
  assert(cfg.replicas == 3);
  assert(cfg.read_quorum == 2);
  assert(cfg.write_quorum == 2);
  assert(cfg.vnodes == 128);
  assert(cfg.scale_rps == 0);
  assert(cfg.scale_window_s == 10);
  assert(cfg.scale_cooldown_s == 60);
  assert(cfg.probe_interval_ms == 1000);
  assert(cfg.probe_fails == 3);
  assert(cfg.forward_timeout_ms == 2000);
  assert(strcmp(cfg.node_image, "kv-storage") == 0);
  assert(cfg.node_base_port == 7900);
  assert(strcmp(cfg.docker_sock, "/var/run/docker.sock") == 0);
  assert(strcmp(cfg.node_network, "kv-net") == 0);
  assert(strcmp(cfg.node_bin, "./build/app") == 0);
  assert(cfg.node_dir == NULL);
  printf("  test_cp_defaults OK\n");
}

static void test_cp_overrides(void) {
  cp_set_required();
  setenv("KV_CP_INITIAL_NODES", "3", 1);
  setenv("KV_CP_MAX_NODES", "5", 1);
  setenv("KV_CP_REPLICAS", "2", 1);
  setenv("KV_CP_READ_QUORUM", "1", 1);
  setenv("KV_CP_WRITE_QUORUM", "2", 1);
  setenv("KV_CP_SCALE_RPS", "500", 1);
  setenv("KV_CP_NODE_IMAGE", "img:1", 1);
  setenv("KV_CP_NODE_BASE_PORT", "8000", 1);
  setenv("KV_CP_DOCKER_SOCK", "/run/docker.sock", 1);
  setenv("KV_CP_NODE_NETWORK", "my_net.1", 1);
  setenv("KV_CP_NODE_BIN", "/usr/bin/kv", 1);
  setenv("KV_CP_NODE_DIR", "/tmp/nodes", 1);
  cp_config cfg;
  assert(cp_config_load(&cfg) == 0);
  assert(strcmp(cfg.docker_sock, "/run/docker.sock") == 0);
  assert(strcmp(cfg.node_network, "my_net.1") == 0);
  assert(strcmp(cfg.node_bin, "/usr/bin/kv") == 0);
  assert(strcmp(cfg.node_dir, "/tmp/nodes") == 0);
  assert(cfg.initial_nodes == 3);
  assert(cfg.max_nodes == 5);
  assert(cfg.replicas == 2);
  assert(cfg.read_quorum == 1);
  assert(cfg.write_quorum == 2);
  assert(cfg.scale_rps == 500);
  assert(strcmp(cfg.node_image, "img:1") == 0);
  assert(cfg.node_base_port == 8000);
  printf("  test_cp_overrides OK\n");
}

static void cp_expect_bad(const char *name, const char *value) {
  cp_set_required();
  setenv(name, value, 1);
  cp_config cfg;
  assert(cp_config_load(&cfg) == -1);
}

static void test_cp_bad(void) {
  cp_set_required();
  unsetenv("KV_CP_DB_PATH");
  cp_config cfg;
  assert(cp_config_load(&cfg) == -1);
  cp_expect_bad("KV_CP_LISTEN_ADDR", "nohostport");
  cp_expect_bad("KV_CP_REPLICAS", "4");
  cp_expect_bad("KV_CP_READ_QUORUM", "0");
  cp_expect_bad("KV_CP_READ_QUORUM", "4");
  cp_expect_bad("KV_CP_WRITE_QUORUM", "0");
  cp_expect_bad("KV_CP_WRITE_QUORUM", "4");
  cp_expect_bad("KV_CP_REPLICAS", "1");
  cp_expect_bad("KV_CP_INITIAL_NODES", "4");
  cp_expect_bad("KV_CP_INITIAL_NODES", "0");
  cp_expect_bad("KV_CP_VNODES", "0");
  cp_expect_bad("KV_CP_PROBE_FAILS", "0");
  cp_expect_bad("KV_CP_SCALE_RPS", "-1");
  cp_expect_bad("KV_CP_NODE_BASE_PORT", "65536");
  cp_expect_bad("KV_CP_DOCKER_SOCK", "docker.sock");
  cp_expect_bad("KV_CP_DOCKER_SOCK",
                "/a/very/long/path/that/does/not/fit/in/sun_path/of/a/unix/socket/"
                "address/which/holds/at/most/108/bytes/with/its/nul.sock");
  cp_expect_bad("KV_CP_NODE_NETWORK", "bad net");
  cp_expect_bad("KV_CP_NODE_NETWORK", "-net");
  cp_set_required();
  setenv("KV_CP_INITIAL_NODES", "17", 1);
  setenv("KV_CP_MAX_NODES", "17", 1);
  setenv("KV_CP_REPLICAS", "17", 1);
  assert(cp_config_load(&cfg) == -1);
  printf("  test_cp_bad OK\n");
}

/** Load the cp config with stderr sent to a file; return what it printed (static buffer). */
static const char *cp_load_stderr(int *rc) {
  static char out[4096];
  FILE *capture = tmpfile();
  assert(capture);
  (void)fflush(stderr);
  int saved = dup(STDERR_FILENO);
  assert(saved >= 0 && dup2(fileno(capture), STDERR_FILENO) >= 0);
  cp_config cfg;
  *rc = cp_config_load(&cfg);
  (void)fflush(stderr);
  assert(dup2(saved, STDERR_FILENO) >= 0);
  close(saved);
  rewind(capture);
  size_t n = fread(out, 1, sizeof(out) - 1, capture);
  out[n] = '\0';
  (void)fclose(capture);
  return out;
}

static void test_cp_warns_when_quorums_may_not_overlap(void) {
  int rc = -1;
  cp_set_required();
  setenv("KV_CP_READ_QUORUM", "1", 1);
  setenv("KV_CP_WRITE_QUORUM", "2", 1);
  assert(strstr(cp_load_stderr(&rc), "stale") != NULL && rc == 0);
  cp_set_required();
  setenv("KV_CP_READ_QUORUM", "2", 1);
  assert(strcmp(cp_load_stderr(&rc), "") == 0 && rc == 0);
  setenv("KV_CP_INITIAL_NODES", "1", 1);
  setenv("KV_CP_REPLICAS", "1", 1);
  setenv("KV_CP_READ_QUORUM", "1", 1);
  setenv("KV_CP_WRITE_QUORUM", "1", 1);
  assert(strcmp(cp_load_stderr(&rc), "") == 0 && rc == 0);
  printf("  test_cp_warns_when_quorums_may_not_overlap OK\n");
}

int main(void) {
  printf("config tests:\n");
  test_required_set_uses_optional_defaults();
  test_overrides();
  test_empty_optional_uses_default();
  test_missing_required();
  test_bad_numbers();
  test_bad_listen_addr();
  test_cp_defaults();
  test_cp_overrides();
  test_cp_bad();
  test_cp_warns_when_quorums_may_not_overlap();
  printf("all config tests passed\n");
  return 0;
}
