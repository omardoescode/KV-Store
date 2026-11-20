#include "cp/cluster.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

int cluster_init(cluster *c, const cp_config *cfg) {
  memset(c, 0, sizeof(*c));
  c->cfg = cfg;
  return pthread_mutex_init(&c->mu, NULL) == 0 ? 0 : -1;
}

void cluster_free(cluster *c) {
  ring_free(c->ring);
  ring_free(c->next);
  ring_free(c->prev);
  pthread_mutex_destroy(&c->mu);
}

ring *cluster_build_ring(cluster *c, int extra) {
  int ids[CLUSTER_MAX_NODES + 1];
  size_t n = 0;
  for (int id = 1; id <= CLUSTER_MAX_NODES; id++) {
    if ((c->nodes[id].exists && c->nodes[id].in_ring) || id == extra) {
      ids[n++] = id;
    }
  }
  return ring_new(ids, n, (ring_params){c->cfg->vnodes, c->cfg->replicas});
}

static uint64_t next_version(cluster *c) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  uint64_t ms = ((uint64_t)ts.tv_sec * 1000) + ((uint64_t)ts.tv_nsec / 1000000);
  uint64_t v = ms << 16U;
  if (v <= c->last_version) {
    v = c->last_version + 1;
  }
  // Above the stored mark, a restart could hand out this version again.
  if (v >= c->version_mark) {
    return 0;
  }
  c->last_version = v;
  return v;
}

static void add_target(plan *p, int id, const cnode *n) {
  plan_target *t = &p->targets[p->count++];
  t->id = id;
  memcpy(t->addr, n->addr, sizeof(t->addr));
}

static void take_ticket(cluster *c, plan *p) {
  p->epoch = c->epoch;
  c->inflight[c->epoch & 1U]++;
}

int cluster_plan_write(cluster *c, const char *key, size_t key_len, plan *out) {
  out->count = 0;
  int prefs[RING_MAX_REPLICAS];
  pthread_mutex_lock(&c->mu);
  out->version = next_version(c);
  size_t n = c->ring && out->version ? ring_prefs(c->ring, key, key_len, prefs) : 0;
  for (size_t i = 0; i != n; i++) {
    cnode *node = &c->nodes[prefs[i]];
    if (node_takes_writes(node->sm.state)) {
      add_target(out, prefs[i], node);
      node->requests++;
    }
  }
  out->replicas = out->count;
  if (out->count == 0) {
    pthread_mutex_unlock(&c->mu);
    return -1;
  }
  if (c->double_write && c->next && ring_has(c->next, c->target, key, key_len)) {
    add_target(out, c->target, &c->nodes[c->target]);
  }
  take_ticket(c, out);
  pthread_mutex_unlock(&c->mu);
  return 0;
}

int cluster_plan_read(cluster *c, const char *key, size_t key_len, plan *out) {
  out->count = 0;
  out->version = 0;
  int prefs[RING_MAX_REPLICAS];
  pthread_mutex_lock(&c->mu);
  size_t n = c->ring ? ring_prefs(c->ring, key, key_len, prefs) : 0;
  for (size_t i = 0; i != n; i++) {
    cnode *node = &c->nodes[prefs[i]];
    if (node_serves_reads(node->sm.state)) {
      add_target(out, prefs[i], node);
    }
  }
  if (out->count != 0) {
    c->nodes[out->targets[0].id].requests++;
    take_ticket(c, out);
  }
  out->replicas = out->count;
  pthread_mutex_unlock(&c->mu);
  return out->count ? 0 : -1;
}

void cluster_done(cluster *c, uint64_t epoch) {
  pthread_mutex_lock(&c->mu);
  c->inflight[epoch & 1U]--;
  pthread_mutex_unlock(&c->mu);
}

void cluster_forward_failed(cluster *c, int id, bool join_target) {
  pthread_mutex_lock(&c->mu);
  c->nodes[id].forward_error = true;
  if (join_target) {
    c->double_write_failed = true;
  }
  pthread_mutex_unlock(&c->mu);
}

int cluster_drain(cluster *c, int timeout_ms) {
  pthread_mutex_lock(&c->mu);
  uint64_t old = c->epoch++;
  pthread_mutex_unlock(&c->mu);
  struct timespec tick = {0, 2000000};
  for (int waited = 0; waited < timeout_ms * 1000; waited += 2000) {
    pthread_mutex_lock(&c->mu);
    long left = c->inflight[old & 1U];
    pthread_mutex_unlock(&c->mu);
    if (left == 0) {
      return 0;
    }
    nanosleep(&tick, NULL);
  }
  return -1;
}
