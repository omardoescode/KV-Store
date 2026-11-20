#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// sizeof(sockaddr_un.sun_path) on Linux, with its NUL.
enum { SUN_PATH_LEN = 108 };

/** Parse name as a long into *out; leave *out unchanged if unset or empty. */
static int env_long(const char *name, long *out) {
  const char *v = getenv(name);
  if (!v || !*v) {
    return 0;
  }
  char *end = NULL;
  errno = 0;
  long n = strtol(v, &end, 10);
  if (errno != 0 || end == v || *end != '\0') {
    (void)fprintf(stderr, "config: %s=\"%s\" is not an integer\n", name, v);
    return -1;
  }
  *out = n;
  return 0;
}

int config_load(kv_config *cfg) {
  static const char *const required[] = {"KV_LISTEN_ADDR", "KV_LOG_PATH", "KV_DB_PATH"};
  const char *values[sizeof(required) / sizeof(required[0])];
  int missing = 0;
  for (size_t i = 0; i != sizeof(required) / sizeof(required[0]); i++) {
    values[i] = getenv(required[i]);
    if (!values[i] || !*values[i]) {
      (void)fprintf(stderr, "config: %s is required but not set\n", required[i]);
      missing = 1;
    }
  }
  if (missing) {
    (void)fprintf(stderr, "config: set the missing vars (see .env.example)\n");
    return -1;
  }
  cfg->listen_addr = values[0];
  cfg->log_path = values[1];
  cfg->db_path = values[2];
  if (!strchr(cfg->listen_addr, ':')) {
    (void)fprintf(stderr, "config: KV_LISTEN_ADDR=\"%s\" must be host:port\n", cfg->listen_addr);
    return -1;
  }

  long flush_ms = 100;
  long threads = 0;
  long max_pending = 1000000;
  long sync_ack = 0;
  long ttl_s = 3600;
  if (env_long("KV_FLUSH_INTERVAL_MS", &flush_ms) != 0 || env_long("KV_THREADS", &threads) != 0 ||
      env_long("KV_MAX_PENDING", &max_pending) != 0 || env_long("KV_SYNC_ACK", &sync_ack) != 0 ||
      env_long("KV_TOMBSTONE_TTL_S", &ttl_s) != 0) {
    return -1;
  }
  if (flush_ms < 1 || flush_ms > 3600000) {
    (void)fprintf(stderr, "config: KV_FLUSH_INTERVAL_MS=%ld must be in [1, 3600000]\n", flush_ms);
    return -1;
  }
  if (threads < 0 || threads > 1024) {
    (void)fprintf(stderr, "config: KV_THREADS=%ld must be in [0, 1024]\n", threads);
    return -1;
  }
  if (max_pending < 1 || max_pending > 1000000000) {
    (void)fprintf(stderr, "config: KV_MAX_PENDING=%ld must be in [1, 1000000000]\n", max_pending);
    return -1;
  }
  if (sync_ack != 0 && sync_ack != 1) {
    (void)fprintf(stderr, "config: KV_SYNC_ACK=%ld must be 0 or 1\n", sync_ack);
    return -1;
  }
  if (ttl_s < 1 || ttl_s > 31536000) {
    (void)fprintf(stderr, "config: KV_TOMBSTONE_TTL_S=%ld must be in [1, 31536000]\n", ttl_s);
    return -1;
  }
  cfg->sync_ack = (int)sync_ack;
  cfg->tombstone_ttl_s = ttl_s;
  cfg->flush_interval_ms = (int)flush_ms;
  cfg->max_pending = max_pending;
  cfg->threads = (int)threads;
  return 0;
}

/** Parse name like env_long, then check it is in [min, max]. */
static int env_range(const char *name, long *out, long min, long max) {
  if (env_long(name, out) != 0) {
    return -1;
  }
  if (*out < min || *out > max) {
    (void)fprintf(stderr, "config: %s=%ld must be in [%ld, %ld]\n", name, *out, min, max);
    return -1;
  }
  return 0;
}

/** Read the settings of the node drivers into cfg. Returns 0 or -1. */
static int load_driver_settings(cp_config *cfg) {
  const char *sock = getenv("KV_CP_DOCKER_SOCK");
  cfg->docker_sock = sock && *sock ? sock : "/var/run/docker.sock";
  if (cfg->docker_sock[0] != '/' || strlen(cfg->docker_sock) >= (size_t)SUN_PATH_LEN) {
    (void)fprintf(stderr,
                  "config: KV_CP_DOCKER_SOCK=\"%s\" must be an absolute path under %zu bytes\n",
                  cfg->docker_sock, (size_t)SUN_PATH_LEN);
    return -1;
  }
  const char *net = getenv("KV_CP_NODE_NETWORK");
  cfg->node_network = net && *net ? net : "kv-net";
  // Docker names: [a-zA-Z0-9][a-zA-Z0-9_.-]*
  static const char name_chars[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-";
  const char *net_name = cfg->node_network;
  if (strspn(net_name, name_chars) != strlen(net_name) || strchr("_.-", net_name[0]) != NULL) {
    (void)fprintf(stderr,
                  "config: KV_CP_NODE_NETWORK=\"%s\" must match [a-zA-Z0-9][a-zA-Z0-9_.-]*\n",
                  cfg->node_network);
    return -1;
  }
  const char *bin = getenv("KV_CP_NODE_BIN");
  cfg->node_bin = bin && *bin ? bin : "./build/app";
  const char *dir = getenv("KV_CP_NODE_DIR");
  cfg->node_dir = dir && *dir ? dir : NULL;
  return 0;
}

int cp_config_load(cp_config *cfg) {
  static const char *const required[] = {"KV_CP_LISTEN_ADDR", "KV_CP_DB_PATH", "KV_CP_LOG_PATH"};
  const char *values[3];
  int missing = 0;
  for (size_t i = 0; i != 3; i++) {
    values[i] = getenv(required[i]);
    if (!values[i] || !*values[i]) {
      (void)fprintf(stderr, "config: %s is required but not set\n", required[i]);
      missing = 1;
    }
  }
  if (missing) {
    return -1;
  }
  cfg->listen_addr = values[0];
  cfg->db_path = values[1];
  cfg->log_path = values[2];
  if (!strchr(cfg->listen_addr, ':')) {
    (void)fprintf(stderr, "config: KV_CP_LISTEN_ADDR=\"%s\" must be host:port\n", cfg->listen_addr);
    return -1;
  }
  long threads = 0;
  long initial = 3;
  long max_nodes = 3;
  long replicas = 3;
  long read_quorum = 2;
  long write_quorum = 2;
  long vnodes = 128;
  long scale_rps = 0;
  long window_s = 10;
  long cooldown_s = 60;
  long probe_ms = 1000;
  long probe_fails = 3;
  long forward_ms = 2000;
  long base_port = 7900;
  if (env_range("KV_CP_THREADS", &threads, 0, 1024) != 0 ||
      env_range("KV_CP_INITIAL_NODES", &initial, 1, 1024) != 0 ||
      env_range("KV_CP_MAX_NODES", &max_nodes, 1, 1024) != 0) {
    return -1;
  }
  // 16 = RING_MAX_REPLICAS: the ring keeps at most 16 replicas of a key.
  if (env_range("KV_CP_REPLICAS", &replicas, 1, 16) != 0 ||
      env_range("KV_CP_READ_QUORUM", &read_quorum, 1, 16) != 0 ||
      env_range("KV_CP_WRITE_QUORUM", &write_quorum, 1, 16) != 0 ||
      env_range("KV_CP_VNODES", &vnodes, 1, 4096) != 0 ||
      env_range("KV_CP_SCALE_RPS", &scale_rps, 0, 1000000000) != 0 ||
      env_range("KV_CP_SCALE_WINDOW_S", &window_s, 1, 3600) != 0 ||
      env_range("KV_CP_SCALE_COOLDOWN_S", &cooldown_s, 0, 86400) != 0 ||
      env_range("KV_CP_PROBE_INTERVAL_MS", &probe_ms, 10, 600000) != 0 ||
      env_range("KV_CP_PROBE_FAILS", &probe_fails, 1, 1000) != 0 ||
      env_range("KV_CP_FORWARD_TIMEOUT_MS", &forward_ms, 1, 600000) != 0 ||
      env_range("KV_CP_NODE_BASE_PORT", &base_port, 1, 65535) != 0) {
    return -1;
  }
  if (replicas > initial) {
    (void)fprintf(stderr, "config: KV_CP_REPLICAS=%ld must be <= KV_CP_INITIAL_NODES=%ld\n",
                  replicas, initial);
    return -1;
  }
  if (initial > max_nodes) {
    (void)fprintf(stderr, "config: KV_CP_INITIAL_NODES=%ld must be <= KV_CP_MAX_NODES=%ld\n",
                  initial, max_nodes);
    return -1;
  }
  if (read_quorum > replicas || write_quorum > replicas) {
    (void)fprintf(stderr,
                  "config: KV_CP_READ_QUORUM=%ld and KV_CP_WRITE_QUORUM=%ld must be <= "
                  "KV_CP_REPLICAS=%ld\n",
                  read_quorum, write_quorum, replicas);
    return -1;
  }
  if (read_quorum + write_quorum <= replicas) {
    (void)fprintf(stderr,
                  "config: warning: KV_CP_READ_QUORUM=%ld + KV_CP_WRITE_QUORUM=%ld <= "
                  "KV_CP_REPLICAS=%ld, so reads may be stale\n",
                  read_quorum, write_quorum, replicas);
  }
  cfg->threads = (int)threads;
  cfg->initial_nodes = (int)initial;
  cfg->max_nodes = (int)max_nodes;
  cfg->replicas = (int)replicas;
  cfg->read_quorum = (int)read_quorum;
  cfg->write_quorum = (int)write_quorum;
  cfg->vnodes = (int)vnodes;
  cfg->scale_rps = scale_rps;
  cfg->scale_window_s = (int)window_s;
  cfg->scale_cooldown_s = (int)cooldown_s;
  cfg->probe_interval_ms = (int)probe_ms;
  cfg->probe_fails = (int)probe_fails;
  cfg->forward_timeout_ms = (int)forward_ms;
  const char *image = getenv("KV_CP_NODE_IMAGE");
  cfg->node_image = image && *image ? image : "kv-storage";
  cfg->node_base_port = (int)base_port;
  return load_driver_settings(cfg);
}
