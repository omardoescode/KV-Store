#include "cp/node_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/hclient.h"
#include "http/http.h"
#include "json/json.h"

enum { DUMP_PAGE = 1000 };

static int call_timeout_ms;
static int health_timeout_ms;

void node_client_init(const cp_config *cfg) {
  call_timeout_ms = cfg->forward_timeout_ms * 5;
  health_timeout_ms = cfg->probe_interval_ms < cfg->forward_timeout_ms ? cfg->probe_interval_ms
                                                                       : cfg->forward_timeout_ms;
}

/** Send a request. Returns the status, or -1. Frees the body unless out is given. */
static int call(const char *addr, const hclient_req *req, hclient_resp *out) {
  hclient_resp resp;
  if (hclient_do(addr, req, &resp) != 0) {
    return -1;
  }
  if (out) {
    *out = resp;
  } else {
    free(resp.body);
  }
  return resp.status;
}

/** POST body (freed) to path. Returns 0 on 200, else -1. */
static int post(const char *addr, char *body, size_t len, const char *path) {
  hclient_req req = {"POST", path, NULL, body, len, call_timeout_ms};
  int status = body ? call(addr, &req, NULL) : -1;
  free(body);
  return status == 200 ? 0 : -1;
}

bool node_health(const char *addr) {
  hclient_req req = {"GET", "/_internal/health", NULL, NULL, 0, health_timeout_ms};
  return call(addr, &req, NULL) == 200;
}

int node_flush(const char *addr) {
  hclient_req req = {"POST", "/_internal/flush", NULL, NULL, 0, call_timeout_ms};
  return call(addr, &req, NULL) == 200 ? 0 : -1;
}

/** Fetch the page after cursor and pass it to fn. Sets *next to the next cursor, or NULL. */
static int dump_page(const char *addr, node_page_fn fn, void *arg, const char *cursor,
                     char **next) {
  *next = NULL;
  size_t path_len = strlen(cursor) + 64;
  char *path = malloc(path_len);
  if (!path) {
    return -1;
  }
  (void)snprintf(path, path_len, "/_internal/dump?limit=%d&cursor=%s", DUMP_PAGE, cursor);
  hclient_req req = {"GET", path, NULL, NULL, 0, call_timeout_ms};
  hclient_resp resp = {0, NULL, 0};
  int status = call(addr, &req, &resp);
  free(path);
  if (status != 200) {
    free(resp.body);
    return -1;
  }
  json_doc *doc = json_parse(resp.body, resp.len);
  free(resp.body);
  const json_val *items = json_get(json_doc_root(doc), "items");
  size_t n = json_len(items);
  node_row *rows = calloc(n ? n : 1, sizeof(*rows));
  if (json_type_of(items) != JSON_ARRAY || !rows) {
    free(rows);
    json_doc_free(doc);
    return -1;
  }
  size_t count = 0;
  for (size_t i = 0; i != n; i++) {
    const json_val *row = json_at(items, i);
    node_row *r = &rows[count];
    r->key = json_str(json_get(row, "key"), &r->key_len);
    r->value = json_str(json_get(row, "value"), &r->value_len);
    if (!r->value) {
      r->value = "";
    }
    size_t clock_len = 0;
    const char *clock = json_str(json_get(row, "clock"), &clock_len);
    r->stamp.ts = (uint64_t)json_int(json_get(row, "ts"));
    r->stamp.coord = (int)json_int(json_get(row, "coord"));
    r->deleted = json_bool(json_get(row, "deleted"));
    if (!clock || vclock_decode(clock, clock_len, &r->stamp.clock) != 0) {
      r->key = NULL;
    }
    count += r->key ? 1 : 0;
  }
  int rc = fn(arg, rows, count) == 0 ? 0 : -1;
  free(rows);
  size_t next_len = 0;
  const char *next_key = json_str(json_get(json_doc_root(doc), "next"), &next_len);
  if (rc == 0 && next_key) {
    *next = malloc((next_len * 3) + 1);
    if (*next) {
      (*next)[http_percent_encode(*next, next_key, next_len)] = '\0';
    } else {
      rc = -1;
    }
  }
  json_doc_free(doc);
  return rc;
}

int node_dump(const char *addr, node_page_fn fn, void *arg) {
  char *cursor = strdup("");
  while (cursor) {
    char *next = NULL;
    int rc = dump_page(addr, fn, arg, cursor, &next);
    free(cursor);
    if (rc != 0 || !next) {
      free(next);
      return rc;
    }
    cursor = next;
  }
  return -1;
}

int node_put_rows(const char *addr, const node_row *rows, size_t n) {
  json_builder *b = json_builder_new();
  json_node *items = json_new_arr(b);
  bool ok = true;
  for (size_t i = 0; ok && i != n; i++) {
    const node_row *r = &rows[i];
    json_node *item = json_new_obj(b);
    char clock[VCLOCK_TEXT_MAX];
    size_t clock_len = vclock_encode(&r->stamp.clock, clock);
    ok = (bool)(json_put(b, item, "key", json_new_str(b, r->key, r->key_len)) &&
                json_put(b, item, "value", json_new_str(b, r->value, r->value_len)) &&
                json_put(b, item, "clock", json_new_str(b, clock, clock_len)) &&
                json_put(b, item, "ts", json_new_int(b, (int64_t)r->stamp.ts)) &&
                json_put(b, item, "coord", json_new_int(b, r->stamp.coord)) &&
                json_put(b, item, "deleted", json_new_bool(b, r->deleted)) &&
                json_push(items, item));
  }
  json_node *root = json_new_obj(b);
  size_t len = 0;
  char *body = ok && json_put(b, root, "items", items) ? json_to_bytes(root, &len) : NULL;
  json_builder_free(b);
  return post(addr, body, len, "/_internal/batch");
}

int node_delete_keys(const char *addr, const node_row *rows, size_t n) {
  json_builder *b = json_builder_new();
  json_node *keys = json_new_arr(b);
  bool ok = true;
  for (size_t i = 0; ok && i != n; i++) {
    ok = json_push(keys, json_new_str(b, rows[i].key, rows[i].key_len));
  }
  json_node *root = json_new_obj(b);
  size_t len = 0;
  char *body = ok && json_put(b, root, "keys", keys) ? json_to_bytes(root, &len) : NULL;
  json_builder_free(b);
  return post(addr, body, len, "/_internal/delete-batch");
}

int node_pause_purge(const char *addr, bool paused) {
  const char *body = paused ? "{\"paused\":true}" : "{\"paused\":false}";
  hclient_req req = {"POST", "/_internal/purge", NULL, body, strlen(body), call_timeout_ms};
  return call(addr, &req, NULL) == 200 ? 0 : -1;
}
