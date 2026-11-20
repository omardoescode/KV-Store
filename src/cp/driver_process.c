// Node driver for tests: each node is a local process of KV_CP_NODE_BIN on
// 127.0.0.1:(KV_CP_NODE_BASE_PORT + id), with its data in KV_CP_NODE_DIR/node-<id>/.
// KV_CP_FAULT="<phase>:<victim>" kills a node (victim = id, or "target") or cp ("cp") when the
// phase starts.
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cp/driver.h"

static const char *node_bin;
static const char *node_dir;
static int base_port;
static logger dlog;

int driver_init(const cp_config *cfg, logger log) {
  dlog = log;
  base_port = cfg->node_base_port;
  node_bin = getenv("KV_CP_NODE_BIN");
  node_dir = getenv("KV_CP_NODE_DIR");
  if (!node_bin || !*node_bin) {
    node_bin = "./build/app";
  }
  if (!node_dir || !*node_dir) {
    (void)fprintf(stderr, "driver: KV_CP_NODE_DIR is required by the process driver\n");
    return -1;
  }
  if (mkdir(node_dir, 0755) != 0 && errno != EEXIST) {
    perror(node_dir);
    return -1;
  }
  // Children are reaped at once; liveness is checked with kill(pid, 0).
  (void)signal(SIGCHLD, SIG_IGN);
  return 0;
}

static void node_path(char *out, size_t len, int id, const char *file) {
  (void)snprintf(out, len, "%s/node-%d%s%s", node_dir, id, file ? "/" : "", file ? file : "");
}

/** Return the pid of node id if it runs, else 0. */
static pid_t node_pid(int id) {
  char path[512];
  node_path(path, sizeof(path), id, "pid");
  FILE *f = fopen(path, "r");
  if (!f) {
    return 0;
  }
  char line[32] = "";
  const char *got = fgets(line, sizeof(line), f);
  (void)fclose(f);
  long pid = got ? strtol(line, NULL, 10) : 0;
  return pid > 0 && kill((pid_t)pid, 0) == 0 ? (pid_t)pid : 0;
}

int driver_spawn(int id, char addr[DRIVER_ADDR_LEN]) {
  (void)snprintf(addr, DRIVER_ADDR_LEN, "127.0.0.1:%d", base_port + id);
  if (node_pid(id) != 0) {
    return 0;
  }
  char dir[512];
  char db[600];
  char log_path[600];
  char pid_path[600];
  node_path(dir, sizeof(dir), id, NULL);
  if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
    return -1;
  }
  (void)snprintf(db, sizeof(db), "%s/kv.db", dir);
  (void)snprintf(log_path, sizeof(log_path), "%s/kv.log", dir);
  (void)snprintf(pid_path, sizeof(pid_path), "%s/pid", dir);
  pid_t pid = fork();
  if (pid < 0) {
    return -1;
  }
  if (pid == 0) {
    sigset_t none;
    sigemptyset(&none);
    pthread_sigmask(SIG_SETMASK, &none, NULL);
    (void)signal(SIGCHLD, SIG_DFL);
    // Own session: a signal to cp's process group must not stop the nodes.
    (void)setsid();
    setenv("KV_LISTEN_ADDR", addr, 1);
    setenv("KV_DB_PATH", db, 1);
    setenv("KV_LOG_PATH", log_path, 1);
    if (!getenv("KV_THREADS")) {
      setenv("KV_THREADS", "2", 1);
    }
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      dup2(devnull, STDIN_FILENO);
      dup2(devnull, STDOUT_FILENO);
    }
    // cp's listen sockets are inherited: a node that outlives cp would hold cp's port open.
    (void)close_range(3, ~0U, 0);
    execl(node_bin, node_bin, (char *)NULL);
    _exit(127);
  }
  FILE *f = fopen(pid_path, "w");
  if (!f) {
    (void)kill(pid, SIGKILL);
    return -1;
  }
  (void)fprintf(f, "%ld\n", (long)pid);
  (void)fclose(f);
  dlog.log(dlog.ctx, LOG_INFO, "driver: node %d started, pid %ld, %s", id, (long)pid, addr);
  return 0;
}

int driver_stop(int id) {
  pid_t pid = node_pid(id);
  if (pid == 0) {
    return 0;
  }
  (void)kill(pid, SIGTERM);
  struct timespec tick = {0, 20000000};
  for (int i = 0; i != 250 && kill(pid, 0) == 0; i++) {
    nanosleep(&tick, NULL);
  }
  if (kill(pid, 0) == 0) {
    (void)kill(pid, SIGKILL);
  }
  char path[512];
  node_path(path, sizeof(path), id, "pid");
  (void)unlink(path);
  return 0;
}

int driver_list(driver_node *out, size_t max) {
  size_t n = 0;
  // Node ids are small; scan a fixed range instead of the directory.
  for (int id = 1; id <= 1024 && n != max; id++) {
    if (node_pid(id) != 0) {
      out[n].id = id;
      (void)snprintf(out[n].addr, DRIVER_ADDR_LEN, "127.0.0.1:%d", base_port + id);
      n++;
    }
  }
  return (int)n;
}

void driver_fault_point(const char *phase, int target) {
  const char *fault = getenv("KV_CP_FAULT");
  if (!fault) {
    return;
  }
  const char *colon = strchr(fault, ':');
  if (!colon || strncmp(fault, phase, (size_t)(colon - fault)) != 0 ||
      strlen(phase) != (size_t)(colon - fault)) {
    return;
  }
  const char *victim = colon + 1;
  dlog.log(dlog.ctx, LOG_WARN, "driver: fault at %s: kill %s", phase, victim);
  if (strcmp(victim, "cp") == 0) {
    _exit(99);
  }
  int id = strcmp(victim, "target") == 0 ? target : (int)strtol(victim, NULL, 10);
  pid_t pid = node_pid(id);
  if (pid != 0) {
    (void)kill(pid, SIGKILL);
    struct timespec tick = {0, 10000000};
    for (int i = 0; i != 200 && kill(pid, 0) == 0; i++) {
      nanosleep(&tick, NULL);
    }
  }
  // Fire once per process: a restarted cp reads the env again, so the test clears it.
  (void)unsetenv("KV_CP_FAULT");
}
