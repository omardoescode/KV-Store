#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config/config.h"
#include "cp/cluster.h"
#include "cp/controller.h"
#include "cp/driver.h"
#include "cp/router.h"
#include "cp/store.h"
#include "http/http.h"
#include "log/async_log.h"

static sem_t server_stopping;

/** Wait for SIGTERM or SIGINT, then stop the server; a second one exits at once. */
static void *wait_for_signal(void *arg) {
  pthread_setname_np(pthread_self(), "cp-signal");
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGINT);
  int sig = 0;
  sigwait(&set, &sig);
  http_server_stop(arg);
  sem_post(&server_stopping);
  sigwait(&set, &sig);
  _exit(1);
}

/** Run the control plane as configured by KV_CP_* env vars until SIGTERM or SIGINT. */
int main(void) {
  cp_config cfg;
  if (cp_config_load(&cfg) != 0) {
    return 1;
  }
  int threads = cfg.threads ? cfg.threads : http_default_threads();
  sigset_t stop_signals;
  sigemptyset(&stop_signals);
  sigaddset(&stop_signals, SIGTERM);
  sigaddset(&stop_signals, SIGINT);
  pthread_sigmask(SIG_BLOCK, &stop_signals, NULL);

  FILE *log_file = strcmp(cfg.log_path, "-") == 0 ? stdout : fopen(cfg.log_path, "a");
  if (!log_file) {
    perror(cfg.log_path);
    return 1;
  }
  // One ring per event loop, plus the controller and the main thread.
  async_log *alog = async_log_new(log_file, (size_t)threads + 2);
  if (!alog) {
    return 1;
  }
  logger log = async_log_logger(alog);
  int status = 1;
  static cluster c;
  cp_store *store = cp_store_open(cfg.db_path);
  controller *ctl = NULL;
  http_server *server = NULL;
  if (!store || cluster_init(&c, &cfg) != 0 || driver_init(&cfg, log) != 0) {
    goto out;
  }
  ctl = controller_new(&c, store, log);
  if (!ctl || controller_start_up(ctl) != 0 || controller_run(ctl) != 0) {
    goto out;
  }
  server = http_server_new();
  cp_router_register(server, &c, &log);
  pthread_t signal_thread;
  if (sem_init(&server_stopping, 0, 0) != 0 ||
      pthread_create(&signal_thread, NULL, wait_for_signal, server) != 0) {
    goto out;
  }
  printf("cp listening on http://%s with %d threads\n", cfg.listen_addr, threads);
  (void)fflush(stdout);
  if (http_listen_and_serve(server, cfg.listen_addr, threads) == 0) {
    status = 0;
  } else {
    perror("http_listen_and_serve");
    (void)kill(getpid(), SIGTERM);
  }
  (void)pthread_detach(signal_thread);
  while (sem_wait(&server_stopping) != 0) {
  }

out:
  controller_free(ctl);
  if (server) {
    http_server_free(server);
  }
  cp_router_free();
  cp_store_close(store);
  async_log_close(alog);
  if (log_file != stdout) {
    (void)fclose(log_file);
  }
  return status;
}
