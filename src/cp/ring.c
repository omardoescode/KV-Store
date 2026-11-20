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

static size_t prefs_at(const ring *r, uint64_t pos, int *out) {
  int rf = r->replicas;
  if (r->count == 0 || rf <= 0) {
    return 0;
  }
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

size_t ring_prefs(const ring *r, const char *key, size_t key_len, int *out) {
  return prefs_at(r, position(key, key_len), out);
}

static int by_value(const void *lhs, const void *rhs) {
  uint64_t a = *(const uint64_t *)lhs;
  uint64_t b = *(const uint64_t *)rhs;
  return (a > b) - (a < b);
}

static bool same_prefs(const ring *from, const ring *to, uint64_t pos) {
  int a[RING_MAX_REPLICAS];
  int b[RING_MAX_REPLICAS];
  size_t na = prefs_at(from, pos, a);
  size_t nb = prefs_at(to, pos, b);
  return (bool)(na == nb && memcmp(a, b, na * sizeof(*a)) == 0);
}

int ring_diff(const ring *from, const ring *to, ring_range **out, size_t *n) {
  *out = NULL;
  *n = 0;
  size_t total = from->count + to->count;
  if (total == 0) {
    return 0;
  }
  // Between two neighbouring points of either ring both preference lists stay the same.
  uint64_t *cuts = malloc(total * sizeof(*cuts));
  ring_range *ranges = malloc(total * sizeof(*ranges));
  if (!cuts || !ranges) {
    free(cuts);
    free(ranges);
    return -1;
  }
  size_t m = 0;
  for (size_t i = 0; i != from->count; i++) {
    cuts[m++] = from->points[i].pos;
  }
  for (size_t i = 0; i != to->count; i++) {
    cuts[m++] = to->points[i].pos;
  }
  qsort(cuts, m, sizeof(*cuts), by_value);
  size_t unique = 1;
  for (size_t i = 1; i != m; i++) {
    if (cuts[i] != cuts[unique - 1]) {
      cuts[unique++] = cuts[i];
    }
  }
  size_t count = 0;
  for (size_t i = 0; i != unique; i++) {
    if (same_prefs(from, to, cuts[i])) {
      continue;
    }
    uint64_t start = cuts[i == 0 ? unique - 1 : i - 1];
    if (count != 0 && ranges[count - 1].end == start) {
      ranges[count - 1].end = cuts[i];
    } else {
      ranges[count++] = (ring_range){start, cuts[i]};
    }
  }
  if (count > 1 && ranges[count - 1].end == ranges[0].start) {
    ranges[0].start = ranges[count - 1].start;
    count--;
  }
  free(cuts);
  if (count == 0) {
    free(ranges);
    ranges = NULL;
  }
  *out = ranges;
  *n = count;
  return 0;
}

bool ring_ranges_have(const ring_range *ranges, size_t n, const char *key, size_t key_len) {
  uint64_t pos = position(key, key_len);
  for (size_t i = 0; i != n; i++) {
    ring_range r = ranges[i];
    if (r.start < r.end) {
      if (pos > r.start && pos <= r.end) {
        return true;
      }
    } else if (pos > r.start || pos <= r.end) {
      return true;
    }
  }
  return false;
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

int xfer_source(const xfer *x, const char *key, size_t len, int target) {
  if (!ring_has(x->to, target, key, len)) {
    return 0;
  }
  int prefs[RING_MAX_REPLICAS];
  size_t n = ring_prefs(x->from, key, len, prefs);
  for (size_t i = 0; i != n; i++) {
    if (prefs[i] != target && x->readers[prefs[i]]) {
      return prefs[i];
    }
  }
  return 0;
}

bool xfer_drops(const xfer *x, int node, const char *key, size_t len) {
  return (bool)(ring_has(x->from, node, key, len) && !ring_has(x->to, node, key, len));
}
