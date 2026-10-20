#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hashmap/hashmap.h"
#include "http/http.h"
#include "json/json.h"

#define ADDR "127.0.0.1:8080"

/** Respond to the ping route with HTTP 200 and a plain-text pong body. */
static void ping(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  http_set_header(w, HTTP_STR("content-type"), HTTP_STR("text/plain"));
  http_write(w, 200, "pong\n", 5);
}

/** Reply to {"msg": "..."} with {"msg": "...", "len": n}, or 400 if the body is not that. */
static void echo(http_response *w, const http_request *r, void *arg) {
  (void)arg;
  http_str body = http_req_body(r);
  json_doc *doc = json_parse(body.ptr, body.len);
  size_t msg_len;
  const char *msg = json_str(json_get(json_doc_root(doc), "msg"), &msg_len);
  if (msg == NULL) {
    json_doc_free(doc);
    static const char usage[] = "want {\"msg\": string}\n";
    http_write(w, 400, usage, sizeof(usage) - 1);
    return;
  }

  json_builder *b = json_builder_new();
  json_node *reply = json_new_obj(b);
  json_put(b, reply, "msg", json_new_str(b, msg, msg_len));
  json_put(b, reply, "len", json_new_int(b, (int64_t)msg_len));
  size_t len;
  char *bytes = json_to_bytes(reply, &len);

  if (bytes != NULL) {
    http_set_header(w, HTTP_STR("content-type"), HTTP_STR("application/json"));
    http_write(w, 200, bytes, len);
  } else {
    http_write(w, 500, NULL, 0);
  }

  free(bytes);
  json_builder_free(b);
  json_doc_free(doc);
}

/** Copy the path's key segment (everything after the leading '/') into a NUL-terminated buffer. */
static char *dup_key(http_str path) {
  if (path.len < 2 || path.ptr[0] != '/') {
    return NULL;
  }
  size_t key_len = path.len - 1;
  char *key = malloc(key_len + 1);
  if (key == NULL) {
    return NULL;
  }
  memcpy(key, path.ptr + 1, key_len);
  key[key_len] = '\0';
  return key;
}

/** POST /{key} with body {"value": string}: store the value under the key. */
static void kv_set(http_response *w, const http_request *r, void *arg) {
  hashmap *m = arg;
  char *key = dup_key(http_req_path(r));
  if (key == NULL) {
    http_write(w, 400, NULL, 0);
    return;
  }

  http_str body = http_req_body(r);
  json_doc *doc = json_parse(body.ptr, body.len);
  size_t val_len;
  const char *val = json_str(json_get(json_doc_root(doc), "value"), &val_len);
  if (val == NULL) {
    static const char usage[] = "want {\"value\": string}\n";
    http_write(w, 400, usage, sizeof(usage) - 1);
    json_doc_free(doc);
    free(key);
    return;
  }

  int rc = hashmap_put(m, key, val, val_len);
  json_doc_free(doc);
  free(key);

  if (rc != 0) {
    http_write(w, 500, NULL, 0);
    return;
  }
  http_write(w, 204, NULL, 0);
}

/** GET /{key}: return {"value": string} on hit, 404 on miss. */
static void kv_get(http_response *w, const http_request *r, void *arg) {
  hashmap *m = arg;
  char *key = dup_key(http_req_path(r));
  if (key == NULL) {
    http_write(w, 400, NULL, 0);
    return;
  }

  void *val = NULL;
  size_t val_len = 0;
  int rc = hashmap_get(m, key, &val, &val_len);
  free(key);
  if (rc < 0) {
    http_write(w, 500, NULL, 0);
    return;
  }
  if (rc == 0) {
    http_write(w, 404, NULL, 0);
    return;
  }

  json_builder *b = json_builder_new();
  json_node *reply = json_new_obj(b);
  json_put(b, reply, "value", json_new_str(b, val != NULL ? val : "", val_len));
  size_t len;
  char *bytes = json_to_bytes(reply, &len);
  free(val);

  if (bytes != NULL) {
    http_set_header(w, HTTP_STR("content-type"), HTTP_STR("application/json"));
    http_write(w, 200, bytes, len);
  } else {
    http_write(w, 500, NULL, 0);
  }

  free(bytes);
  json_builder_free(b);
}

/** Serve ping, echo, and KV set/get; return 1 if serving stops. */
int main(void) {
  hashmap *m = hashmap_new();
  if (m == NULL) {
    perror("hashmap_new");
    return 1;
  }

  http_server *server = http_server_new();
  http_handle(server, HTTP_GET, "/ping", ping, NULL);
  http_handle(server, HTTP_POST, "/echo", echo, NULL);
  http_handle(server, HTTP_GET, "/", kv_get, m);
  http_handle(server, HTTP_POST, "/", kv_set, m);

  int threads = http_default_threads();

  printf("listening on http://" ADDR " with %d threads\n", threads);
  if (http_listen_and_serve(server, ADDR, threads) != 0) {
    perror("http_listen_and_serve");
  }
  hashmap_free(m);
  return 1;
}
