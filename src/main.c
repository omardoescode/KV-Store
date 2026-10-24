#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config.h"
#include "http/http.h"
#include "kv/kv_handlers.h"
#include "kv/kv_store.h"
#include "log/async_log.h"

static sem_t server_stopping;

/**
 * Wait for SIGTERM or SIGINT, which every thread blocks, then stop the server. A second one
 * exits at once, dropping writes not yet committed, in case the graceful stop hangs.
 */
static void *wait_for_signal(void *arg) {
  pthread_setname_np(pthread_self(), "kv-signal");
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  int sig = 0;
  sigwait(&set, &sig);
  http_server_stop(arg);
  sem_post(&server_stopping);
  sigwait(&set, &sig);
  static const char message[] = "second signal: exiting without a clean stop\n";
  (void)!write(STDERR_FILENO, message, sizeof(message) - 1);
  _exit(1);
}

/** Serve the KV endpoints as configured by KV_* env vars until SIGTERM or SIGINT. */
int main(void) {
  kv_config cfg;
  if (config_load(&cfg) != 0) {
    return 1;
  }
  int threads = cfg.threads ? cfg.threads : http_default_threads();

  // Block before any thread starts so that all inherit the mask and only sigwait takes them.
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGTERM);
  sigaddset(&stop_signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);

  int log_to_stdout = strcmp(cfg.log_path, "-") == 0;
  FILE *log_file = log_to_stdout ? stdout : fopen(cfg.log_path, "a");
  if (!log_file) {
    perror(cfg.log_path);
    return 1;
  }
  // One ring per event loop, plus one for the kv_store writer thread.
  async_log *alog = async_log_new(log_file, (size_t)threads + 1);
  if (!alog) {
    (void)fprintf(stderr, "Failed to start the logger.\n");
    (void)fclose(log_file);
    return 1;
  }
  logger log = async_log_logger(alog);

  kv_store *store = kv_store_new(cfg.db_path, cfg.flush_interval_ms, log, (size_t)cfg.max_pending);
  if (!store) {
    (void)fprintf(stderr, "Failed to create kv_store.\n");
    async_log_close(alog);
    (void)fclose(log_file);
    return 1;
  }

  http_server *server = http_server_new();
  kv_register_routes(server, store, &log);

  pthread_t signal_thread;
  int status = 1;
  if (sem_init(&server_stopping, 0, 0) != 0 ||
      pthread_create(&signal_thread, NULL, wait_for_signal, server) != 0) {
    (void)fprintf(stderr, "Failed to start the signal thread.\n");
  } else {
    printf("listening on http://%s with %d threads\n", cfg.listen_addr, threads);
    (void)fflush(stdout);
    if (http_listen_and_serve(server, cfg.listen_addr, threads) == 0) {
      status = 0;
    } else {
      perror("http_listen_and_serve");
      (void)kill(getpid(), SIGTERM);
    }
    // Not joined: the signal thread waits for a second signal until the process exits.
    (void)pthread_detach(signal_thread);
    while (sem_wait(&server_stopping) != 0) {
    }
  }

  if (kv_store_free(store) != 0) {
    status = 1;
  }
  http_server_free(server);
  async_log_close(alog);
  (void)fclose(log_file);
  return status;
}
