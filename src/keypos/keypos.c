#include "keypos/keypos.h"

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
