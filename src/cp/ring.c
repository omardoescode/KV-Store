#include "cp/ring.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "siphash.h"

typedef struct {
  uint64_t pos;
  int id;
} point;

struct ring {
  point *points;
  size_t count;
  size_t node_count;
  int replicas;
};

// The ASCII bytes of "kv-store-ring-01". Changing them moves every key.
static const uint8_t ring_key[16] = {
    'k', 'v', '-', 's', 't', 'o', 'r', 'e', '-', 'r', 'i', 'n', 'g', '-', '0', '1',
};

static uint64_t position(const void *data, size_t len) {
  uint8_t out[8];
  (void)siphash(data, len, ring_key, out, sizeof(out));
  uint64_t v;
  memcpy(&v, out, sizeof(v));
  return v;
}

static int by_position(const void *lhs, const void *rhs) {
  const point *pa = lhs;
  const point *pb = rhs;
  if (pa->pos != pb->pos) {
    return pa->pos < pb->pos ? -1 : 1;
  }
  // Equal positions are near impossible; order by id so the ring stays deterministic.
  return (pa->id > pb->id) - (pa->id < pb->id);
}

ring *ring_new(const int *ids, size_t n, ring_params params) {
  int vnodes = params.vnodes;
  ring *r = calloc(1, sizeof(*r));
  if (!r) {
    return NULL;
  }
  r->node_count = n;
  r->replicas = params.replicas < RING_MAX_REPLICAS ? params.replicas : RING_MAX_REPLICAS;
  r->count = n * (size_t)vnodes;
  if (r->count == 0) {
    return r;
  }
  r->points = malloc(r->count * sizeof(*r->points));
  if (!r->points) {
    free(r);
    return NULL;
  }
  size_t k = 0;
  for (size_t i = 0; i != n; i++) {
    for (int v = 0; v != vnodes; v++) {
      char name[32];
      int len = snprintf(name, sizeof(name), "%d#%d", ids[i], v);
      r->points[k++] = (point){position(name, (size_t)len), ids[i]};
    }
  }
  qsort(r->points, r->count, sizeof(*r->points), by_position);
  return r;
}

void ring_free(ring *r) {
  if (r) {
    free(r->points);
    free(r);
  }
}

size_t ring_prefs(const ring *r, const char *key, size_t key_len, int *out) {
  int rf = r->replicas;
  if (r->count == 0 || rf <= 0) {
    return 0;
  }
  uint64_t pos = position(key, key_len);
  size_t lo = 0;
  size_t hi = r->count;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) / 2);
    if (r->points[mid].pos < pos) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  size_t want = (size_t)rf < r->node_count ? (size_t)rf : r->node_count;
  size_t found = 0;
  for (size_t step = 0; step != r->count && found != want; step++) {
    int id = r->points[(lo + step) % r->count].id;
    int seen = 0;
    for (size_t j = 0; j != found && !seen; j++) {
      seen = out[j] == id;
    }
    if (!seen) {
      out[found++] = id;
    }
  }
  return found;
}

int ring_has(const ring *r, int id, const char *key, size_t key_len) {
  int prefs[RING_MAX_REPLICAS];
  size_t n = ring_prefs(r, key, key_len, prefs);
  for (size_t i = 0; i != n; i++) {
    if (prefs[i] == id) {
      return 1;
    }
  }
  return 0;
}
