#ifndef CONFIG_H
#define CONFIG_H

/** Server settings read from KV_* environment variables. Strings point into the environment. */
typedef struct {
  const char *listen_addr;
  const char *log_path;
  const char *db_path;
  const char *snapshot_path;
  int flush_interval_ms;
  /** 0 means one event loop per usable CPU. */
  int threads;
} kv_config;

/**
 * Fill cfg from the environment, using defaults for unset or empty variables.
 * Returns 0 on success, -1 after printing the bad variable to stderr.
 */
int config_load(kv_config *cfg);

#endif /* CONFIG_H */
