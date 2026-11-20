#include "cp/router.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/fwd.h"
#include "json/json.h"

enum { KEYS_BUCKETS = 4096 };

typedef enum { OP_GET, OP_POST, OP_DELETE } op_kind;

typedef struct op {
  struct op *next;
  cluster *c;
  logger *log;
  http_deferred *d;
  op_kind kind;
  int timeout_ms;
  plan p;
  bool planned;
  // Read: index of the target tried now. Write: forwards not answered yet.
  size_t at;
  size_t pending;
  // Write: status and body of the first replica, and whether a replica failed.
  int status;
  char *resp;
  size_t resp_len;
  bool failed;
  size_t key_len;
  char *key;
  char *path;
  size_t body_len;
  char body[];
} op;

// Ops of one key: head runs, the rest wait in order.
typedef struct key_entry {
  struct key_entry *next;
  op *head;
  op *tail;
  uint64_t hash;
} key_entry;

static pthread_mutex_t keys_mu = PTHREAD_MUTEX_INITIALIZER;
static key_entry *keys[KEYS_BUCKETS];

static uint64_t hash_key(const char *key, size_t len) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i != len; i++) {
    h = (h ^ (unsigned char)key[i]) * 1099511628211ULL;
  }
  return h;
}

static bool same_key(const op *a, const op *b) {
  return (bool)(a->key_len == b->key_len && memcmp(a->key, b->key, a->key_len) == 0);
}

static void run_op(void *arg);

/** Queue o behind ops on its key; return whether o may run now. */
static bool keys_enter(op *o) {
  uint64_t h = hash_key(o->key, o->key_len);
  key_entry **bucket = &keys[h % KEYS_BUCKETS];
  pthread_mutex_lock(&keys_mu);
  for (key_entry *e = *bucket; e; e = e->next) {
    if (e->hash == h && same_key(e->head, o)) {
      e->tail->next = o;
      e->tail = o;
      pthread_mutex_unlock(&keys_mu);
      return false;
    }
  }
  key_entry *e = malloc(sizeof(*e));
  if (!e) {
    pthread_mutex_unlock(&keys_mu);
    // No table entry: run unordered rather than fail.
    return true;
  }
  *e = (key_entry){*bucket, o, o, h};
  *bucket = e;
  pthread_mutex_unlock(&keys_mu);
  return true;
}

/** Remove o, the head of its key; start the next op of the key on its own loop. */
static void keys_leave(op *o) {
  uint64_t h = hash_key(o->key, o->key_len);
  op *next = NULL;
  pthread_mutex_lock(&keys_mu);
  for (key_entry **link = &keys[h % KEYS_BUCKETS]; *link; link = &(*link)->next) {
    key_entry *e = *link;
    if (e->head == o) {
      next = o->next;
      if (next) {
        e->head = next;
      } else {
        *link = e->next;
        free(e);
      }
      break;
    }
  }
  pthread_mutex_unlock(&keys_mu);
  if (next) {
    http_deferred_post(next->d, run_op, next);
  }
}

static const char *const kind_names[] = {"GET", "POST", "DELETE"};

static void finish(op *o, int status, const char *body, size_t len) {
  static const char unavailable[] = "no live replica\n";
  if (status == 503 && !body) {
    body = unavailable;
    len = sizeof(unavailable) - 1;
  }
  http_header json = {HTTP_STR("content-type"), HTTP_STR("application/json")};
  bool is_json = (bool)(o->kind == OP_GET && status == 200);
  http_resume(o->d, status, is_json ? &json : NULL, is_json ? 1 : 0, body, len);
  o->log->log(o->log->ctx, LOG_INFO, "%s %s %d", kind_names[o->kind], o->path, status);
  if (o->planned) {
    cluster_done(o->c, o->p.epoch);
  }
  keys_leave(o);
  free(o->resp);
  free(o->path);
  free(o);
}

static void read_next(op *o);

static void on_read(void *ctx, const fwd_result *res) {
  op *o = ctx;
  if (res->status >= 0 && res->status < 500) {
    finish(o, res->status, res->body, res->len);
    return;
  }
  if (res->status < 0) {
    cluster_forward_failed(o->c, o->p.targets[o->at].id, false);
  }
  o->at++;
  read_next(o);
}

static void read_next(op *o) {
  if (o->at == o->p.count) {
    finish(o, 503, NULL, 0);
    return;
  }
  fwd_req req = {"GET", o->path, NULL, NULL, 0, o->timeout_ms};
  fwd_send(o->p.targets[o->at].addr, &req, on_read, o);
}

typedef struct {
  op *o;
  size_t index;
} write_ctx;

static void on_written(void *ctx, const fwd_result *res) {
  write_ctx *w = ctx;
  op *o = w->o;
  size_t i = w->index;
  free(w);
  bool join_target = i >= o->p.replicas;
  bool ok = (bool)(res->status >= 0 && res->status < 500);
  if (!ok) {
    if (res->status < 0 || join_target) {
      cluster_forward_failed(o->c, o->p.targets[i].id, join_target);
    }
    if (!join_target) {
      o->failed = true;
    }
  }
  if (i == 0 && ok) {
    o->status = res->status;
    o->resp = res->len ? malloc(res->len) : NULL;
    if (o->resp) {
      memcpy(o->resp, res->body, res->len);
      o->resp_len = res->len;
    }
  }
  if (--o->pending != 0) {
    return;
  }
  if (o->failed) {
    finish(o, 503, NULL, 0);
  } else {
    finish(o, o->status, o->resp, o->resp_len);
  }
}

static void write_all(op *o) {
  char headers[64];
  (void)snprintf(headers, sizeof(headers), "x-kv-version: %" PRIu64 "\r\n", o->p.version);
  fwd_req req = {
      o->kind == OP_POST ? "POST" : "DELETE", o->path, headers, o->body, o->body_len, o->timeout_ms,
  };
  o->pending = o->p.count;
  for (size_t i = 0; i != o->p.count; i++) {
    write_ctx *w = malloc(sizeof(*w));
    if (!w) {
      abort();
    }
    *w = (write_ctx){o, i};
    fwd_send(o->p.targets[i].addr, &req, on_written, w);
  }
}

static void run_op(void *arg) {
  op *o = arg;
  int rc = o->kind == OP_GET ? cluster_plan_read(o->c, o->key, o->key_len, &o->p)
                             : cluster_plan_write(o->c, o->key, o->key_len, &o->p);
  if (rc != 0) {
    finish(o, 503, NULL, 0);
    return;
  }
  o->planned = true;
  if (o->kind == OP_GET) {
    read_next(o);
  } else {
    write_all(o);
  }
}

typedef struct {
  cluster *c;
  const cp_config *cfg;
  logger *log;
} route_ctx;

static route_ctx ctx_storage;

static void handle_key(http_response *w, const http_request *r, op_kind kind) {
  route_ctx *rc = &ctx_storage;
  http_str key = http_req_param(r, HTTP_STR("key"));
  if (memchr(key.ptr, '\0', key.len) != NULL) {
    static const char message[] = "key must not contain NUL\n";
    http_write(w, 400, message, sizeof(message) - 1);
    return;
  }
  if (!json_valid_utf8((const unsigned char *)key.ptr, key.len)) {
    static const char message[] = "key must be UTF-8\n";
    http_write(w, 400, message, sizeof(message) - 1);
    return;
  }
  http_str body = kind == OP_POST ? http_req_body(r) : (http_str){NULL, 0};
  op *o = calloc(1, sizeof(*o) + body.len + key.len + 1);
  char *path = malloc((key.len * 3) + 2);
  if (path) {
    path[0] = '/';
    path[1 + http_percent_encode(path + 1, key.ptr, key.len)] = '\0';
  }
  if (!o || !path) {
    free(o);
    free(path);
    http_write(w, 500, NULL, 0);
    return;
  }
  o->c = rc->c;
  o->log = rc->log;
  o->kind = kind;
  o->timeout_ms = rc->cfg->forward_timeout_ms;
  o->body_len = body.len;
  if (body.len) {
    memcpy(o->body, body.ptr, body.len);
  }
  o->key = o->body + body.len;
  memcpy(o->key, key.ptr, key.len);
  o->key_len = key.len;
  o->path = path;
  o->d = http_defer(w);
  if (keys_enter(o)) {
    run_op(o);
  }
}

static void get_handler(http_response *w, const http_request *r, void *arg) {
  (void)arg;
  handle_key(w, r, OP_GET);
}

static void post_handler(http_response *w, const http_request *r, void *arg) {
  (void)arg;
  handle_key(w, r, OP_POST);
}

static void delete_handler(http_response *w, const http_request *r, void *arg) {
  (void)arg;
  handle_key(w, r, OP_DELETE);
}

static void write_json(http_response *w, int status, json_node *root) {
  size_t len = 0;
  char *bytes = root ? json_to_bytes(root, &len) : NULL;
  if (!bytes) {
    http_write(w, 500, NULL, 0);
    return;
  }
  http_set_header(w, HTTP_STR("content-type"), HTTP_STR("application/json"));
  http_write(w, status, bytes, len);
  free(bytes);
}

static void nodes_handler(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  cluster_view *v = malloc(sizeof(*v));
  if (!v) {
    http_write(w, 500, NULL, 0);
    return;
  }
  cluster_snapshot(ctx_storage.c, v);
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  json_node *arr = json_new_arr(b);
  bool ok = json_put(b, root, "nodes", arr);
  for (int id = 1; ok && id <= CLUSTER_MAX_NODES; id++) {
    const cluster_node *n = &v->nodes[id];
    if (!n->exists) {
      continue;
    }
    json_node *item = json_new_obj(b);
    const char *state = node_state_name(n->sm.state);
    ok = (bool)(json_put(b, item, "id", json_new_int(b, id)) &&
                json_put(b, item, "addr", json_new_str(b, n->addr, strlen(n->addr))) &&
                json_put(b, item, "state", json_new_str(b, state, strlen(state))) &&
                json_put(b, item, "in_ring", json_new_bool(b, n->in_ring)) && json_push(arr, item));
  }
  free(v);
  write_json(w, 200, ok ? root : NULL);
  json_builder_free(b);
}

static void ring_handler(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  cluster_view *v = malloc(sizeof(*v));
  if (!v) {
    http_write(w, 500, NULL, 0);
    return;
  }
  cluster_snapshot(ctx_storage.c, v);
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  json_node *arr = json_new_arr(b);
  bool ok = true;
  for (int id = 1; ok && id <= CLUSTER_MAX_NODES; id++) {
    if (v->nodes[id].exists && v->nodes[id].in_ring) {
      ok = json_push(arr, json_new_int(b, id));
    }
  }
  free(v);
  ok = (bool)(ok && json_put(b, root, "nodes", arr) &&
              json_put(b, root, "vnodes", json_new_int(b, ctx_storage.cfg->vnodes)) &&
              json_put(b, root, "replicas", json_new_int(b, ctx_storage.cfg->replicas)));
  write_json(w, 200, ok ? root : NULL);
  json_builder_free(b);
}

static void migration_handler(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  cluster_view *v = malloc(sizeof(*v));
  if (!v) {
    http_write(w, 500, NULL, 0);
    return;
  }
  cluster_snapshot(ctx_storage.c, v);
  mig_phase phase = v->phase;
  int target = v->target;
  free(v);
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  const char *name = mig_phase_name(phase);
  bool ok =
      (bool)(json_put(b, root, "phase", json_new_str(b, name, strlen(name))) &&
             json_put(b, root, "target", target ? json_new_int(b, target) : json_new_null(b)));
  write_json(w, 200, ok ? root : NULL);
  json_builder_free(b);
}

/** POST /_cp/nodes: ask the controller to add one node; 409 at KV_CP_MAX_NODES. */
static void add_node_handler(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  if (!cluster_request_add(ctx_storage.c)) {
    static const char message[] = "KV_CP_MAX_NODES reached\n";
    http_write(w, 409, message, sizeof(message) - 1);
    return;
  }
  static const char body[] = "{\"queued\":true}";
  http_set_header(w, HTTP_STR("content-type"), HTTP_STR("application/json"));
  http_write(w, 202, body, sizeof(body) - 1);
}

void cp_router_register(http_server *server, cluster *c, const cp_config *cfg, logger *log) {
  ctx_storage = (route_ctx){c, cfg, log};
  http_handle(server, HTTP_GET, "/{key}", get_handler, NULL);
  http_handle(server, HTTP_POST, "/{key}", post_handler, NULL);
  http_handle(server, HTTP_DELETE, "/{key}", delete_handler, NULL);
  http_handle(server, HTTP_GET, "/_cp/nodes", nodes_handler, NULL);
  http_handle(server, HTTP_POST, "/_cp/nodes", add_node_handler, NULL);
  http_handle(server, HTTP_GET, "/_cp/ring", ring_handler, NULL);
  http_handle(server, HTTP_GET, "/_cp/migration", migration_handler, NULL);
  http_on_worker_exit(server, fwd_thread_free);
}
void cp_router_free(void) {
  pthread_mutex_lock(&keys_mu);
  for (size_t i = 0; i != KEYS_BUCKETS; i++) {
    while (keys[i]) {
      key_entry *e = keys[i];
      keys[i] = e->next;
      free(e);
    }
  }
  pthread_mutex_unlock(&keys_mu);
}
