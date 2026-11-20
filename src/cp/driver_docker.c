// Node driver for production: each node is a container kv-node-<id> of KV_CP_NODE_IMAGE on the
// Docker network KV_CP_NODE_NETWORK (default kv-net), with its data on volume kv-data-<id>.
// Talks to the Docker Engine API over KV_CP_DOCKER_SOCK (default /var/run/docker.sock).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/driver.h"
#include "cp/hclient.h"
#include "cp/resolve.h"
#include "json/json.h"

enum { DOCKER_TIMEOUT_MS = 30000, NODE_PORT = 7890 };

static char sock_addr[256];
static const char *image;
static const char *network;
static logger dlog;

// Node settings that cp passes on to each container when set in its own environment.
static const char *const passed_env[] = {
    "KV_SYNC_ACK", "KV_TOMBSTONE_TTL_S", "KV_FLUSH_INTERVAL_MS", "KV_MAX_PENDING", "KV_THREADS",
};

int driver_init(const cp_config *cfg, logger log) {
  dlog = log;
  image = cfg->node_image;
  network = cfg->node_network;
  (void)snprintf(sock_addr, sizeof(sock_addr), "unix:%s", cfg->docker_sock);
  return 0;
}

/** Call the Docker API. Returns the status, or -1. Sets *out to the body if out is not NULL. */
static int docker(const char *method, const char *path, const char *body, hclient_resp *out) {
  hclient_req req = {method, path, NULL, body, body ? strlen(body) : 0, DOCKER_TIMEOUT_MS};
  hclient_resp resp;
  if (hclient_do(sock_addr, &req, &resp) != 0) {
    dlog.log(dlog.ctx, LOG_ERROR, "driver: docker %s %s failed", method, path);
    return -1;
  }
  if (out) {
    *out = resp;
  } else {
    free(resp.body);
  }
  return resp.status;
}

/** Write the "ip:port" of container kv-node-<id> to addr. Returns 0 or -1. */
static int node_addr(int id, char addr[DRIVER_ADDR_LEN]) {
  char host_port[32];
  (void)snprintf(host_port, sizeof(host_port), "kv-node-%d:%d", id, NODE_PORT);
  return resolve_addr(host_port, addr);
}

/** Build the create body of node id. Returns a malloc string, or NULL. */
static char *create_body(int id) {
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  json_node *env = json_new_arr(b);
  char line[256];
  bool ok = (bool)(json_push(env, json_new_str(b, "KV_LISTEN_ADDR=0.0.0.0:7890", 27)) &&
                   json_push(env, json_new_str(b, "KV_LOG_PATH=-", 13)) &&
                   json_push(env, json_new_str(b, "KV_DB_PATH=/data/kv.db", 22)));
  for (size_t i = 0; ok && i != sizeof(passed_env) / sizeof(passed_env[0]); i++) {
    const char *v = getenv(passed_env[i]);
    if (v && *v) {
      int n = snprintf(line, sizeof(line), "%s=%s", passed_env[i], v);
      ok = (bool)(n > 0 && (size_t)n < sizeof(line) &&
                  json_push(env, json_new_str(b, line, (size_t)n)));
    }
  }
  json_node *labels = json_new_obj(b);
  int n = snprintf(line, sizeof(line), "%d", id);
  ok = (bool)(ok && json_put(b, labels, "kv.role", json_new_str(b, "node", 4)) &&
              json_put(b, labels, "kv.id", json_new_str(b, line, (size_t)n)));
  json_node *host = json_new_obj(b);
  json_node *binds = json_new_arr(b);
  n = snprintf(line, sizeof(line), "kv-data-%d:/data", id);
  ok = (bool)(ok && json_push(binds, json_new_str(b, line, (size_t)n)) &&
              json_put(b, host, "Binds", binds) &&
              json_put(b, host, "NetworkMode", json_new_str(b, network, strlen(network))) &&
              json_put(b, root, "Image", json_new_str(b, image, strlen(image))) &&
              json_put(b, root, "Env", env) && json_put(b, root, "Labels", labels) &&
              json_put(b, root, "HostConfig", host));
  size_t len = 0;
  char *bytes = ok ? json_to_bytes(root, &len) : NULL;
  json_builder_free(b);
  return bytes;
}

int driver_spawn(int id, char addr[DRIVER_ADDR_LEN]) {
  char path[128];
  (void)snprintf(path, sizeof(path), "/containers/kv-node-%d/json", id);
  hclient_resp resp = {0, NULL, 0};
  if (docker("GET", path, NULL, &resp) == 200) {
    json_doc *doc = json_parse(resp.body, resp.len);
    bool running = json_bool(json_get(json_get(json_doc_root(doc), "State"), "Running"));
    json_doc_free(doc);
    free(resp.body);
    if (running) {
      return node_addr(id, addr);
    }
    (void)snprintf(path, sizeof(path), "/containers/kv-node-%d?force=true", id);
    (void)docker("DELETE", path, NULL, NULL);
  } else {
    free(resp.body);
  }
  char *body = create_body(id);
  (void)snprintf(path, sizeof(path), "/containers/create?name=kv-node-%d", id);
  int status = body ? docker("POST", path, body, NULL) : -1;
  free(body);
  if (status != 201) {
    dlog.log(dlog.ctx, LOG_ERROR, "driver: create of kv-node-%d: status %d", id, status);
    return -1;
  }
  (void)snprintf(path, sizeof(path), "/containers/kv-node-%d/start", id);
  status = docker("POST", path, NULL, NULL);
  if (status != 204 && status != 304) {
    dlog.log(dlog.ctx, LOG_ERROR, "driver: start of kv-node-%d: status %d", id, status);
    return -1;
  }
  dlog.log(dlog.ctx, LOG_INFO, "driver: kv-node-%d started", id);
  return node_addr(id, addr);
}

int driver_stop(int id) {
  char path[128];
  (void)snprintf(path, sizeof(path), "/containers/kv-node-%d?force=true", id);
  int status = docker("DELETE", path, NULL, NULL);
  return status == 204 || status == 404 ? 0 : -1;
}

int driver_list(driver_node *out, size_t max) {
  hclient_resp resp;
  // filters={"label":["kv.role=node"]}
  if (docker("GET", "/containers/json?filters=%7B%22label%22%3A%5B%22kv.role%3Dnode%22%5D%7D", NULL,
             &resp) != 200) {
    return -1;
  }
  json_doc *doc = json_parse(resp.body, resp.len);
  free(resp.body);
  const json_val *arr = json_doc_root(doc);
  size_t n = 0;
  for (size_t i = 0; i != json_len(arr) && n != max; i++) {
    const char *id = json_str(json_get(json_get(json_at(arr, i), "Labels"), "kv.id"), NULL);
    long v = id ? strtol(id, NULL, 10) : 0;
    if (v > 0 && v <= 1024 && node_addr((int)v, out[n].addr) == 0) {
      out[n].id = (int)v;
      n++;
    }
  }
  json_doc_free(doc);
  return (int)n;
}

void driver_fault_point(const char *phase, int target) {
  (void)phase;
  (void)target;
}
