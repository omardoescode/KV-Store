#ifndef CONFIG_H
#define CONFIG_H

/** Server settings read from KV_* environment variables. Strings point into the environment. */
typedef struct {
  const char *listen_addr;
  const char *log_path;
  const char *db_path;
  int flush_interval_ms;
  /** Changes that may wait for the database before POST/DELETE fail with 503. */
  long max_pending;
  /** 0 means one event loop per usable CPU. */
  int threads;
  /** 1: POST/DELETE respond only after the change is committed. */
  int sync_ack;
  /** Seconds a delete marker (tombstone) stays before the purge removes it. */
  long tombstone_ttl_s;
} kv_config;

/**
 * Fill cfg from the environment, using defaults for unset or empty variables.
 * Returns 0 on success, -1 after printing the bad variable to stderr.
 */
int config_load(kv_config *cfg);

/** Control plane settings read from KV_CP_* environment variables. */
typedef struct {
  const char *listen_addr;
  const char *db_path;
  const char *log_path;
  /** 0 means one event loop per usable CPU. */
  int threads;
  int initial_nodes;
  /** Not used yet: no scale-down. */
  int min_nodes;
  int max_nodes;
  int replicas;
  int vnodes;
  /** RPS of one node over the window that adds a node; 0 turns autoscale off. */
  long scale_rps;
  int scale_window_s;
  int scale_cooldown_s;
  int probe_interval_ms;
  int probe_fails;
  int forward_timeout_ms;
  /** Docker image of a storage node (Docker driver). */
  const char *node_image;
  /** A node listens on 127.0.0.1:(node_base_port + id) (process driver). */
  int node_base_port;
} cp_config;

/**
 * Fill cfg from the environment like config_load. Also fails if REPLICAS > INITIAL_NODES,
 * INITIAL_NODES > MAX_NODES or MIN_NODES > MAX_NODES.
 */
int cp_config_load(cp_config *cfg);

#endif /* CONFIG_H */
