#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config/config.h"
#include "http/http.h"
#include "kv/kv_handlers.h"
#include "kv/kv_store.h"
#include "log/async_log.h"
#include "json/json.h"

/** Respond to the ping route with HTTP 200 and a plain-text pong body. */
static void ping(http_response *w, const http_request *r, void *arg) {
  (void)r;
  (void)arg;
  http_set_header(w, HTTP_STR("content-type"), HTTP_STR("text/plain"));
  http_write(w, 200, "pong\n", 5);
}

/** Serve the ping and KV endpoints as configured by KV_* env vars; return 1 if serving stops. */
int main(void) {
  kv_config cfg;
  if (config_load(&cfg) != 0) {
    return 1;
  }
  int threads = cfg.threads ? cfg.threads : http_default_threads();

  int log_to_stdout = strcmp(cfg.log_path, "-") == 0;
  FILE *log_file = log_to_stdout ? stdout : fopen(cfg.log_path, "a");
  if (!log_file) {
    perror(cfg.log_path);
    return 1;
  }
  async_log *alog = async_log_new(log_file, (size_t)threads);
  if (!alog) {
    (void)fprintf(stderr, "Failed to start the logger.\n");
    (void)fclose(log_file);
    return 1;
  }
  logger log = async_log_logger(alog);

  kv_store *store = kv_store_new(cfg.snapshot_path, cfg.flush_interval_ms);
  if (!store) {
    (void)fprintf(stderr, "Failed to create kv_store.\n");
    async_log_close(alog);
    (void)fclose(log_file);
    return 1;
  }

  http_server *server = http_server_new();
  http_handle(server, HTTP_GET, "/ping", ping, NULL);
  kv_register_routes(server, store, &log);

  printf("listening on http://%s with %d threads\n", cfg.listen_addr, threads);
  if (http_listen_and_serve(server, cfg.listen_addr, threads) != 0) {
    perror("http_listen_and_serve");
  }

  kv_store_free(store);
  http_server_free(server);
  async_log_close(alog);
  (void)fclose(log_file);
  return 1;
}
