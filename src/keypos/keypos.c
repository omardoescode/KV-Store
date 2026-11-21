#include "keypos/keypos.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "siphash.h"

// The ASCII bytes of "kv-store-ring-01".
static const uint8_t ring_key[16] = {
    'k', 'v', '-', 's', 't', 'o', 'r', 'e', '-', 'r', 'i', 'n', 'g', '-', '0', '1',
};

uint64_t key_position(const void *data, size_t len) {
  uint8_t out[8];
  (void)siphash(data, len, ring_key, out, sizeof(out));
  uint64_t v;
  memcpy(&v, out, sizeof(v));
  return v;
}

size_t key_segment(uint64_t pos, const uint64_t *ends, size_t n) {
  size_t lo = 0;
  size_t hi = n;
  while (lo < hi) {
    size_t mid = lo + ((hi - lo) / 2);
    if (ends[mid] < pos) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo == n ? 0 : lo;
}

uint64_t key_entry_hash(const char *key, size_t key_len, const vstamp *stamp, bool deleted) {
  char clock[VCLOCK_TEXT_MAX];
  (void)vclock_encode(&stamp->clock, clock);
  char tail[VCLOCK_TEXT_MAX + 64];
  int tail_len = snprintf(tail, sizeof(tail), "\n%s\n%" PRIu64 "\n%d\n%d", clock, stamp->ts,
                          stamp->coord, deleted ? 1 : 0);
  size_t len = key_len + (size_t)tail_len;
  char *buf = malloc(len);
  if (!buf) {
    return 0;
  }
  memcpy(buf, key, key_len);
  memcpy(buf + key_len, tail, (size_t)tail_len);
  uint64_t h = key_position(buf, len);
  free(buf);
  return h;
}
