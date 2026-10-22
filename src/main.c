#include <stdio.h>
#include <stdlib.h>

#include "http/http.h"
#include "json/json.h"
#include "kv/kv_handlers.h"
#include "kv/kv_store.h"

#define ADDR "127.0.0.1:7890"

/** Respond to the ping route with HTTP 200 and a plain-text pong body. */
static void ping(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  http_set_header(w, HTTP_STR("content-type"), HTTP_STR("text/plain"));
  http_write(w, 200, "pong\n", 5);
}

/** Serve the ping and echo endpoints with one worker per usable CPU; return 1 if serving stops. */
int main(void) {
  http_server *server = http_server_new();
  http_handle(server, HTTP_GET, "/ping", ping, NULL);

  kv_store *store = kv_store_new("kv_snapshot_persistence.txt");
  if (!store) {
    fprintf(stderr, "Failed to create kv_store.\n");
    return 1;
  }
  kv_register_routes(server, store);

  int threads = http_default_threads();

  printf("listening on http://" ADDR " with %d threads\n", threads);
  if (http_listen_and_serve(server, ADDR, threads) != 0) {
    perror("http_listen_and_serve");
  }

  kv_store_free(store);
  return 1;
}
