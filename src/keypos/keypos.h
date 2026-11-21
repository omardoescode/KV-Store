#ifndef KEYPOS_KEYPOS_H
#define KEYPOS_KEYPOS_H

#include <stddef.h>
#include <stdint.h>

/**
 * The ring position of len bytes of data: SipHash under a fixed key, so cp and the nodes agree.
 * Changing the key moves every key.
 */
uint64_t key_position(const void *data, size_t len);

/**
 * Whether pos lies in the ring segment (start, end], wrapping past the top; start == end is the
 * whole ring.
 */
static inline int key_in_segment(uint64_t pos, uint64_t start, uint64_t end) {
  if (start < end) {
    return pos > start && pos <= end;
  }
  return pos > start || pos <= end;
}

#endif
