#ifndef VCLOCK_VCLOCK_H
#define VCLOCK_VCLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Most entries a clock holds: one per node that coordinated a write of the key. As in Dynamo, a
 * clock over the limit drops its entries with the oldest ts. A dropped entry can make a later
 * version look concurrent with an earlier one; the winner rule of vstamp_pick then picks by ts.
 */
enum { VCLOCK_MAX = 32 };

typedef struct {
  /** Node id, >= 1. */
  int id;
  /** >= 1. */
  uint64_t n;
  /** Wall-clock ms of the write that set n. */
  uint64_t ts;
} vclock_entry;

/** A vector clock: entries in id order; a missing id counts as 0. */
typedef struct {
  size_t len;
  vclock_entry e[VCLOCK_MAX];
} vclock;

typedef enum { VCLOCK_EQUAL, VCLOCK_BEFORE, VCLOCK_AFTER, VCLOCK_CONCURRENT } vclock_order;

/** Return how a orders against b: BEFORE if b dominates a, AFTER if a dominates b. */
vclock_order vclock_compare(const vclock *a, const vclock *b);

/** Set each entry of into to the entry with the larger n of it and the entry in other. */
void vclock_merge(vclock *into, const vclock *other);

/** Add 1 to the entry of node id and set its ts. */
void vclock_increment(vclock *c, int id, uint64_t ts);

/** The version of one write: its clock, the wall-clock timestamp and the coordinator id. */
typedef struct {
  vclock clock;
  uint64_t ts;
  int coord;
} vstamp;

/**
 * The fixed winner rule. A clock that dominates wins; else (equal or concurrent clocks) the higher
 * ts wins, then the higher coord, then a. Sets *out to the winner with the merge of both clocks.
 * Returns 0 if a wins, 1 if b wins.
 */
int vstamp_pick(const vstamp *a, const vstamp *b, vstamp *out);

/** Room for the text of any clock, with its NUL: "id:n:ts," is at most 10 + 1 + 20 + 1 + 20 + 1. */
enum { VCLOCK_TEXT_MAX = (VCLOCK_MAX * 53) + 1 };

/** Write the text of c (see vclock_decode) and a NUL to out. Returns the text length. */
size_t vclock_encode(const vclock *c, char out[VCLOCK_TEXT_MAX]);

/**
 * Parse len bytes of text, "id:n:ts" entries joined by ',' in increasing id order ("" is the empty
 * clock), into *out. Returns 0, or -1 if malformed or over VCLOCK_MAX entries.
 */
int vclock_decode(const char *text, size_t len, vclock *out);

#endif
