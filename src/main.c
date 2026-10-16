#include <stdio.h>
#include <stdlib.h>

#include "http/http.h"
#include "json/json.h"

#define ADDR "127.0.0.1:7890"

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

/** Serve the ping and echo endpoints with one worker per usable CPU; return 1 if serving stops. */
int main(void) {
  http_server *server = http_server_new();
  http_handle(server, HTTP_GET, "/ping", ping, NULL);
  http_handle(server, HTTP_POST, "/echo", echo, NULL);

  int threads = http_default_threads();

  printf("listening on http://" ADDR " with %d threads\n", threads);
  if (http_listen_and_serve(server, ADDR, threads) != 0) {
    perror("http_listen_and_serve");
  }
  return 1;
}
