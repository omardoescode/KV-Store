#include "cp/router.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/fwd.h"
#include "vclock/vclock.h"
#include "vclock/vstamp_json.h"
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
  // The client has its response; the op lives on until its forwards answer.
  bool responded;
  size_t outstanding;
  // Read phase: reads not answered yet, the answers, and per target whether it answered, with no
  // entry (missing) or with an entry of stamps[i]. decided: the first read_quorum answers are in;
  // later answers only get a read repair.
  size_t reads_out;
  size_t answers;
  bool decided;
  bool answered[RING_MAX_REPLICAS + 1];
  bool missing[RING_MAX_REPLICAS + 1];
  vstamp stamps[RING_MAX_REPLICAS + 1];
  // The winner of the first read_quorum answers by the fixed rule, with the merge of their clocks.
  bool found;
  vstamp win;
  bool win_deleted;
  char *win_value;
  size_t win_len;
  // A read wrote the winner back to every target that did not answer with it.
  bool wrote_back;
  // Write phase: home acks, home writes not answered yet, the acks needed, and the response on
  // success.
  size_t acks;
  size_t home_out;
  size_t acks_needed;
  int status;
  size_t key_len;
  char *key;
  char *path;
  char *get_path;
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

/** Free o once the client has its response and every forward answered. */
static void release(op *o) {
  if (!o->responded || o->outstanding != 0) {
    return;
  }
  if (o->planned) {
    cluster_done(o->c, o->p.epoch);
  }
  free(o->win_value);
  free(o->get_path);
  free(o->path);
  free(o);
}

static const char not_found[] = "Key not found.\n";

/** Send the response, once, and let the next op of the key start. */
static void respond(op *o, int status, const char *body, size_t len) {
  if (o->responded) {
    return;
  }
  static const char unavailable[] = "no quorum of replicas\n";
  if (status == 503 && !body) {
    body = unavailable;
    len = sizeof(unavailable) - 1;
  }
  if (status == 404 && !body) {
    body = not_found;
    len = sizeof(not_found) - 1;
  }
  http_header json = {HTTP_STR("content-type"), HTTP_STR("application/json")};
  http_header retry = {HTTP_STR("retry-after"), HTTP_STR("1")};
  const http_header *header = NULL;
  if (o->kind == OP_GET && status == 200) {
    header = &json;
  } else if (status == 503) {
    header = &retry;
  }
  http_resume(o->d, status, header, header ? 1 : 0, body, len);
  o->log->log(o->log->ctx, LOG_INFO, "%s %s %d", kind_names[o->kind], o->path, status);
  o->responded = true;
  keys_leave(o);
}

/** Return the malloc'd JSON {"value": ...} of the winner, or NULL on error. */
static char *winner_json(const op *o, size_t *len) {
  json_builder *b = json_builder_new();
  json_node *root = json_new_obj(b);
  char *bytes =
      json_put(b, root, "value", json_new_str(b, o->win_value ? o->win_value : "", o->win_len))
          ? json_to_bytes(root, len)
          : NULL;
  json_builder_free(b);
  return bytes;
}

/** Respond with the winner: its value, or 404 if it is a tombstone or there is none. */
static void respond_winner(op *o) {
  if (!o->found || o->win_deleted) {
    respond(o, 404, NULL, 0);
    return;
  }
  size_t len = 0;
  char *bytes = winner_json(o, &len);
  respond(o, bytes ? 200 : 500, bytes, len);
  free(bytes);
}

typedef struct {
  op *o;
  size_t index;
} fwd_ctx;

static void send_to(op *o, size_t i, const fwd_req *req, fwd_cb cb) {
  fwd_ctx *f = malloc(sizeof(*f));
  if (!f) {
    abort();
  }
  *f = (fwd_ctx){o, i};
  o->outstanding++;
  fwd_send(o->p.targets[i].addr, req, cb, f);
}

/** Send the entry (stamp, deleted, body: the JSON of a put) to target i; cb gets the answer. */
static void send_entry(op *o, size_t i, const vstamp *stamp, bool deleted, const char *body,
                       size_t body_len, fwd_cb cb) {
  char clock[VCLOCK_TEXT_MAX];
  (void)vclock_encode(&stamp->clock, clock);
  char headers[VCLOCK_TEXT_MAX + 96];
  (void)snprintf(headers, sizeof(headers),
                 "x-kv-clock: %s\r\nx-kv-ts: %" PRIu64 "\r\nx-kv-coord: %d\r\n", clock, stamp->ts,
                 stamp->coord);
  fwd_req req = {
      deleted ? "DELETE" : "POST", o->path,       headers, deleted ? NULL : body,
      deleted ? 0 : body_len,      o->timeout_ms,
  };
  send_to(o, i, &req, cb);
}

static void on_written(void *ctx, const fwd_result *res) {
  fwd_ctx *f = ctx;
  op *o = f->o;
  size_t i = f->index;
  free(f);
  o->outstanding--;
  bool home = i < o->p.replicas;
  bool ok = (bool)(res->status == 200 || res->status == 404);
  if (!ok && (bool)(res->status < 0 || !home)) {
    cluster_forward_failed(o->c, o->p.targets[i].id, (bool)!home);
  }
  if (home) {
    o->home_out--;
    o->acks += ok ? 1 : 0;
  }
  if (o->acks >= o->acks_needed) {
    if (o->kind == OP_GET) {
      respond_winner(o);
    } else {
      respond(o, o->status, NULL, 0);
    }
  } else if (o->acks + o->home_out < o->acks_needed) {
    respond(o, 503, NULL, 0);
  }
  release(o);
}

/**
 * Write the entry (stamp, deleted, body: the JSON of a put) to every target but those in skip,
 * and respond once W home replicas hold it, counting o->acks already there.
 */
static void write_entry(op *o, const vstamp *stamp, bool deleted, const char *body, size_t body_len,
                        const bool *skip) {
  o->acks_needed = o->p.write_quorum;
  o->home_out = 0;
  for (size_t i = 0; i != o->p.replicas; i++) {
    o->home_out += skip[i] ? 0 : 1;
  }
  if (o->acks + o->home_out < o->acks_needed) {
    respond(o, 503, NULL, 0);
    return;
  }
  for (size_t i = 0; i != o->p.count; i++) {
    if (!skip[i]) {
      send_entry(o, i, stamp, deleted, body, body_len, on_written);
    }
  }
}

/** Write a new version of the key over the winner of the read phase. */
static void write_new(op *o) {
  static const bool none[RING_MAX_REPLICAS + 1];
  vstamp stamp = {.ts = o->p.ts, .coord = o->p.coord};
  if (o->found) {
    stamp.clock = o->win.clock;
  }
  vclock_increment(&stamp.clock, o->p.coord, o->p.ts);
  bool existed = (bool)(o->found && !o->win_deleted);
  o->status = o->kind == OP_DELETE && !existed ? 404 : 200;
  write_entry(o, &stamp, o->kind == OP_DELETE, o->body, o->body_len, none);
}

/**
 * ABD write-back: if a replica that answered is behind the winner, write the winner to the
 * others before the read returns; replicas that answered with it count as acks. Replicas that
 * answer later get it too then.
 */
static void read_done(op *o) {
  bool fresh[RING_MAX_REPLICAS + 1] = {false};
  size_t n_fresh = 0;
  for (size_t i = 0; o->found && i != o->p.replicas; i++) {
    fresh[i] = (bool)(o->answered[i] && !o->missing[i] && vstamp_same(&o->stamps[i], &o->win));
    n_fresh += fresh[i] ? 1 : 0;
  }
  if (!o->found || n_fresh == o->answers) {
    respond_winner(o);
    return;
  }
  size_t len = 0;
  char *body = o->win_deleted ? NULL : winner_json(o, &len);
  if (!o->win_deleted && !body) {
    respond(o, 500, NULL, 0);
    return;
  }
  o->wrote_back = true;
  o->acks = n_fresh;
  write_entry(o, &o->win, o->win_deleted, body, len, fresh);
  free(body);
}

static void on_repaired(void *ctx, const fwd_result *res) {
  (void)res;
  fwd_ctx *f = ctx;
  op *o = f->o;
  free(f);
  o->outstanding--;
  release(o);
}

/**
 * Read repair of target i, which answered after the read returned: if it is behind the winner,
 * write the winner to it.
 */
static void repair_late(op *o, size_t i) {
  vstamp merged;
  if (o->kind != OP_GET || !o->found || o->wrote_back ||
      (!o->missing[i] && (vstamp_same(&o->stamps[i], &o->win) ||
                          vstamp_pick(&o->win, &o->stamps[i], &merged) != 0))) {
    return;
  }
  size_t len = 0;
  char *body = o->win_deleted ? NULL : winner_json(o, &len);
  if (o->win_deleted || body) {
    send_entry(o, i, &o->win, o->win_deleted, body, len, on_repaired);
  }
  free(body);
}

/**
 * Take the answer of target i, a JSON entry, into stamps[i] and, before the decision, into the
 * winner. Returns 0 or -1 if malformed.
 */
static int take_answer(op *o, size_t i, const char *body, size_t len) {
  json_doc *doc = json_parse(body, len);
  const json_val *root = json_doc_root(doc);
  size_t value_len = 0;
  const char *value = json_str(json_get(root, "value"), &value_len);
  vstamp *s = &o->stamps[i];
  int rc = value && vstamp_from_json(root, s) ? 0 : -1;
  if (rc == 0 && !o->decided) {
    vstamp merged = *s;
    bool wins = (bool)(!o->found || vstamp_pick(&o->win, s, &merged) == 1);
    o->win = merged;
    o->found = true;
    char *copy = wins ? malloc(value_len ? value_len : 1) : NULL;
    if (copy) {
      memcpy(copy, value, value_len);
      free(o->win_value);
      o->win_value = copy;
      o->win_len = value_len;
      o->win_deleted = json_bool(json_get(root, "deleted"));
    } else if (wins) {
      rc = -1;
    }
  }
  json_doc_free(doc);
  return rc;
}

static void on_read(void *ctx, const fwd_result *res) {
  fwd_ctx *f = ctx;
  op *o = f->o;
  size_t i = f->index;
  free(f);
  o->outstanding--;
  o->reads_out--;
  bool ok = (bool)(res->status == 404 ||
                   (res->status == 200 && take_answer(o, i, res->body, res->len) == 0));
  if (ok) {
    o->answered[i] = true;
    o->missing[i] = res->status == 404;
  } else if (res->status < 0) {
    cluster_forward_failed(o->c, o->p.targets[i].id, false);
  }
  if (o->decided) {
    if (ok) {
      repair_late(o, i);
    }
  } else if (!o->responded) {
    if (ok && ++o->answers == o->p.read_quorum) {
      o->decided = true;
      if (o->kind == OP_GET) {
        read_done(o);
      } else {
        write_new(o);
      }
    } else if (o->answers + o->reads_out < o->p.read_quorum) {
      respond(o, 503, NULL, 0);
    }
  }
  release(o);
}

static void run_op(void *arg) {
  op *o = arg;
  int rc = o->kind == OP_GET ? cluster_plan_read(o->c, o->key, o->key_len, &o->p)
                             : cluster_plan_write(o->c, o->key, o->key_len, &o->p);
  if (rc != 0) {
    respond(o, 503, NULL, 0);
    release(o);
    return;
  }
  o->planned = true;
  // A write reads the clocks of the replicas it writes to first, so with 2W > N its clock
  // dominates every acked write.
  fwd_req req = {"GET", o->get_path, NULL, NULL, 0, o->timeout_ms};
  for (size_t i = 0; i != o->p.replicas; i++) {
    if (o->p.targets[i].reads) {
      o->reads_out++;
      send_to(o, i, &req, on_read);
    }
  }
  release(o);
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
  if (kind == OP_POST) {
    json_doc *doc = json_parse(body.ptr, body.len);
    bool valid = json_type_of(json_get(json_doc_root(doc), "value")) == JSON_STRING;
    json_doc_free(doc);
    if (!valid) {
      static const char message[] = "want {\"value\": string}\n";
      http_write(w, 400, message, sizeof(message) - 1);
      return;
    }
  }
  op *o = calloc(1, sizeof(*o) + body.len + key.len + 1);
  char *path = malloc((key.len * 3) + 2);
  static const char get_prefix[] = "/_internal/get?key=";
  char *get_path = malloc(sizeof(get_prefix) + (key.len * 3));
  if (path && get_path) {
    path[0] = '/';
    path[1 + http_percent_encode(path + 1, key.ptr, key.len)] = '\0';
    (void)snprintf(get_path, sizeof(get_prefix) + (key.len * 3), "%s%s", get_prefix, path + 1);
  }
  if (!o || !path || !get_path) {
    free(o);
    free(path);
    free(get_path);
    http_write(w, 500, NULL, 0);
    return;
  }
  o->get_path = get_path;
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
