#ifndef KEYPOS_KEYPOS_H
#define KEYPOS_KEYPOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vclock/vclock.h"

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

/**
 * Return the index of the segment of pos among the n segments that end at the increasing positions
 * ends: segment i holds (ends[i - 1], ends[i]], segment 0 wrapping past the top.
 */
size_t key_segment(uint64_t pos, const uint64_t *ends, size_t n);

/**
 * The hash of one entry, a tombstone too, in a segment digest (the XOR of its entries' hashes):
 * equal entries give equal hashes on every node.
 */
uint64_t key_entry_hash(const char *key, size_t key_len, const vstamp *stamp, bool deleted);

#endif
