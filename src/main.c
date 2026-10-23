#include <stdio.h>
#include <stdlib.h>

#include "http/http.h"
#include "kv/kv_handlers.h"
#include "kv/kv_store.h"
#include "log/async_log.h"
#include "json/json.h"

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

  int threads = http_default_threads();

  FILE *log_file = fopen("kv.log", "a");
  if (!log_file) {
    perror("Could not open kv.log.\n");
    return 1;
  }
  async_log *alog = async_log_new(log_file, (size_t)threads);
  if (!alog) {
    (void)fprintf(stderr, "Failed to start the logger.\n");
    (void)fclose(log_file);
    return 1;
  }
  logger log = async_log_logger(alog);

  kv_store *store = kv_store_new("kv_snapshot_persistence.txt");
  if (!store) {
    (void)fprintf(stderr, "Failed to create kv_store.\n");
    async_log_close(alog);
    (void)fclose(log_file);
    return 1;
  }
  kv_register_routes(server, store, &log);

  printf("listening on http://" ADDR " with %d threads\n", threads);
  if (http_listen_and_serve(server, ADDR, threads) != 0) {
    perror("http_listen_and_serve");
  }

  kv_store_free(store);
  http_server_free(server);
  async_log_close(alog);
  (void)fclose(log_file);
  return 1;
}
