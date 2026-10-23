#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
  if (env_long("KV_FLUSH_INTERVAL_MS", &flush_ms) != 0 || env_long("KV_THREADS", &threads) != 0) {
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
  cfg->flush_interval_ms = (int)flush_ms;
  cfg->threads = (int)threads;
  return 0;
}
