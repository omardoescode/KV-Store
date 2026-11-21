#include "cp/node_client.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cp/hclient.h"
#include "http/http.h"
#include "vclock/vstamp_json.h"
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

/**
 * GET path, a page of rows {"items":[{key,value,clock,ts,coord,deleted}]}, and pass its rows to
 * fn. Returns 0 or -1; sets *out to the parsed page, which the caller frees, or NULL.
 */
static int fetch_rows(const char *addr, node_page_fn fn, void *arg, const char *path,
                      json_doc **out) {
  *out = NULL;
  hclient_req req = {"GET", path, NULL, NULL, 0, call_timeout_ms};
  hclient_resp resp = {0, NULL, 0};
  int status = call(addr, &req, &resp);
  if (status != 200) {
    free(resp.body);
    return -1;
  }
  json_doc *doc = json_parse(resp.body, resp.len);
  free(resp.body);
  *out = doc;
  const json_val *items = json_get(json_doc_root(doc), "items");
  size_t n = json_len(items);
  node_row *rows = calloc(n ? n : 1, sizeof(*rows));
  if (json_type_of(items) != JSON_ARRAY || !rows) {
    free(rows);
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
    r->deleted = json_bool(json_get(row, "deleted"));
    if (!vstamp_from_json(row, &r->stamp)) {
      r->key = NULL;
    }
    count += r->key ? 1 : 0;
  }
  int rc = fn(arg, rows, count) == 0 ? 0 : -1;
  free(rows);
  return rc;
}

/** Fetch the dump page after cursor and pass it to fn. Sets *next to the next cursor, or NULL. */
static int dump_page(const char *addr, node_page_fn fn, void *arg, const char *cursor,
                     char **next) {
  *next = NULL;
  size_t path_len = strlen(cursor) + 64;
  char *path = malloc(path_len);
  if (!path) {
    return -1;
  }
  (void)snprintf(path, path_len, "/_internal/dump?limit=%d&cursor=%s", DUMP_PAGE, cursor);
  json_doc *doc = NULL;
  int rc = fetch_rows(addr, fn, arg, path, &doc);
  free(path);
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

int node_range(const char *addr, uint64_t start, uint64_t end, node_page_fn fn, void *arg) {
  char path[96];
  (void)snprintf(path, sizeof(path), "/_internal/range?start=%" PRIu64 "&end=%" PRIu64, start, end);
  json_doc *doc = NULL;
  int rc = fetch_rows(addr, fn, arg, path, &doc);
  json_doc_free(doc);
  return rc;
}

/** POST {"ends": ends, "purge": purge (if not NULL)} to path. Returns the parsed 200 answer, or
 * NULL. */
static json_doc *post_ends(const char *addr, const uint64_t *ends, const bool *purge, size_t n,
                           const char *path) {
  json_builder *b = json_builder_new();
  json_node *arr = json_new_arr(b);
  json_node *root = json_new_obj(b);
  bool ok = json_put(b, root, "ends", arr);
  for (size_t i = 0; ok && i != n; i++) {
    ok = json_push(arr, json_new_int(b, (int64_t)ends[i]));
  }
  json_node *flags = purge ? json_new_arr(b) : NULL;
  ok = (bool)(ok && (!purge || json_put(b, root, "purge", flags)));
  for (size_t i = 0; ok && purge && i != n; i++) {
    ok = json_push(flags, json_new_bool(b, purge[i]));
  }
  size_t len = 0;
  char *body = ok ? json_to_bytes(root, &len) : NULL;
  json_builder_free(b);
  if (!body) {
    return NULL;
  }
  hclient_req req = {"POST", path, NULL, body, len, call_timeout_ms};
  hclient_resp resp = {0, NULL, 0};
  int status = call(addr, &req, &resp);
  free(body);
  json_doc *doc = status == 200 ? json_parse(resp.body, resp.len) : NULL;
  free(resp.body);
  return doc;
}

int node_digest(const char *addr, const uint64_t *ends, size_t n, uint64_t *hashes) {
  json_doc *doc = post_ends(addr, ends, NULL, n, "/_internal/digest");
  const json_val *got = json_get(json_doc_root(doc), "hashes");
  int rc = json_type_of(got) == JSON_ARRAY && json_len(got) == n ? 0 : -1;
  for (size_t i = 0; rc == 0 && i != n; i++) {
    hashes[i] = (uint64_t)json_int(json_at(got, i));
  }
  json_doc_free(doc);
  return rc;
}

int node_purge(const char *addr, const uint64_t *ends, const bool *purge, size_t n) {
  json_doc *doc = post_ends(addr, ends, purge, n, "/_internal/purge");
  int rc = json_type_of(json_get(json_doc_root(doc), "purged")) == JSON_NUMBER ? 0 : -1;
  json_doc_free(doc);
  return rc;
}

int node_put_rows(const char *addr, const node_row *rows, size_t n) {
  json_builder *b = json_builder_new();
  json_node *items = json_new_arr(b);
  bool ok = true;
  for (size_t i = 0; ok && i != n; i++) {
    const node_row *r = &rows[i];
    json_node *item = json_new_obj(b);
    ok = (bool)(json_put(b, item, "key", json_new_str(b, r->key, r->key_len)) &&
                json_put(b, item, "value", json_new_str(b, r->value, r->value_len)) &&
                vstamp_to_json(b, item, &r->stamp) &&
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
