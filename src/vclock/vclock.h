#ifndef VCLOCK_VCLOCK_H
#define VCLOCK_VCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Most entries a clock holds: one per node that coordinated a write of the key. */
enum { VCLOCK_MAX = 32 };

typedef struct {
  /** Node id, >= 1. */
  int id;
  /** >= 1. */
  uint64_t n;
} vclock_entry;

/** A vector clock: entries in id order; a missing id counts as 0. */
typedef struct {
  size_t len;
  vclock_entry e[VCLOCK_MAX];
} vclock;

typedef enum { VCLOCK_EQUAL, VCLOCK_BEFORE, VCLOCK_AFTER, VCLOCK_CONCURRENT } vclock_order;

/** Return how a orders against b: BEFORE if b dominates a, AFTER if a dominates b. */
vclock_order vclock_compare(const vclock *a, const vclock *b);

/** Set each entry of into to the max of it and the entry in other. Returns 0, or -1 (into
 * unchanged) if the result has over VCLOCK_MAX entries. */
int vclock_merge(vclock *into, const vclock *other);

/** Add 1 to the entry of node id. Returns 0, or -1 (c unchanged) if c is full. */
int vclock_increment(vclock *c, int id);

/** The version of one write: its clock, the wall-clock timestamp and the coordinator id. */
typedef struct {
  vclock clock;
  uint64_t ts;
  int coord;
} vstamp;

/**
 * The fixed winner rule. A clock that dominates wins; else (equal or concurrent clocks) the higher
 * ts wins, then the higher coord, then a. Sets *out to the winner with the merge of both clocks.
 * Returns 0 if a wins, 1 if b wins, -1 if the merge is over VCLOCK_MAX entries.
 */
int vstamp_pick(const vstamp *a, const vstamp *b, vstamp *out);

/** Room for the text of any clock, with its NUL: "id:n," is at most 10 + 1 + 20 + 1 bytes. */
enum { VCLOCK_TEXT_MAX = (VCLOCK_MAX * 32) + 1 };

/** Write the text of c (see vclock_decode) and a NUL to out. Returns the text length. */
size_t vclock_encode(const vclock *c, char out[VCLOCK_TEXT_MAX]);

/**
 * Parse len bytes of text, "id:n" entries joined by ',' in increasing id order ("" is the empty
 * clock), into *out. Returns 0, or -1 if malformed or over VCLOCK_MAX entries.
 */
int vclock_decode(const char *text, size_t len, vclock *out);

#endif
