#include "vclock/vclock.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

vclock_order vclock_compare(const vclock *a, const vclock *b) {
  bool a_more = false;
  bool b_more = false;
  size_t i = 0;
  size_t j = 0;
  while (i != a->len || j != b->len) {
    if (j == b->len || (i != a->len && a->e[i].id < b->e[j].id)) {
      a_more = true;
      i++;
    } else if (i == a->len || b->e[j].id < a->e[i].id) {
      b_more = true;
      j++;
    } else {
      a_more = (bool)(a_more || a->e[i].n > b->e[j].n);
      b_more = (bool)(b_more || b->e[j].n > a->e[i].n);
      i++;
      j++;
    }
  }
  if (a_more && b_more) {
    return VCLOCK_CONCURRENT;
  }
  if (a_more) {
    return VCLOCK_AFTER;
  }
  return b_more ? VCLOCK_BEFORE : VCLOCK_EQUAL;
}

/** Drop the entries with the oldest ts until at most VCLOCK_MAX are left. */
static void prune(vclock_entry *e, size_t *len) {
  while (*len > VCLOCK_MAX) {
    size_t old = 0;
    for (size_t i = 1; i != *len; i++) {
      if (e[i].ts < e[old].ts) {
        old = i;
      }
    }
    memmove(&e[old], &e[old + 1], (*len - old - 1) * sizeof(*e));
    (*len)--;
  }
}

void vclock_merge(vclock *into, const vclock *other) {
  vclock_entry out[VCLOCK_MAX * 2];
  size_t len = 0;
  size_t i = 0;
  size_t j = 0;
  while (i != into->len || j != other->len) {
    if (j == other->len || (i != into->len && into->e[i].id < other->e[j].id)) {
      out[len++] = into->e[i++];
    } else if (i == into->len || other->e[j].id < into->e[i].id) {
      out[len++] = other->e[j++];
    } else {
      vclock_entry a = into->e[i++];
      vclock_entry b = other->e[j++];
      out[len++] = b.n > a.n || (b.n == a.n && b.ts > a.ts) ? b : a;
    }
  }
  prune(out, &len);
  memcpy(into->e, out, len * sizeof(*out));
  into->len = len;
}

void vclock_increment(vclock *c, int id, uint64_t ts) {
  for (size_t i = 0; i != c->len; i++) {
    if (c->e[i].id == id) {
      c->e[i].n++;
      c->e[i].ts = ts;
      return;
    }
  }
  vclock one = {1, {{id, 1, ts}}};
  vclock_merge(c, &one);
}

/** Return whether b comes after a in a total order of clocks: entry by entry, id, then n, then ts.
 */
static bool clock_after(const vclock *a, const vclock *b) {
  for (size_t i = 0; i != a->len && i != b->len; i++) {
    const vclock_entry *x = &a->e[i];
    const vclock_entry *y = &b->e[i];
    if (x->id != y->id) {
      return y->id < x->id;
    }
    if (x->n != y->n || x->ts != y->ts) {
      return (bool)(y->n > x->n || (y->n == x->n && y->ts > x->ts));
    }
  }
  return b->len > a->len;
}

int vstamp_pick(const vstamp *a, const vstamp *b, vstamp *out) {
  vclock_order order = vclock_compare(&a->clock, &b->clock);
  bool b_wins = order == VCLOCK_BEFORE;
  if (order == VCLOCK_EQUAL || order == VCLOCK_CONCURRENT) {
    b_wins = (bool)(b->ts > a->ts || (b->ts == a->ts && b->coord > a->coord) ||
                    (b->ts == a->ts && b->coord == a->coord && clock_after(&a->clock, &b->clock)));
  }
  vclock merged = a->clock;
  vclock_merge(&merged, &b->clock);
  *out = b_wins ? *b : *a;
  out->clock = merged;
  return b_wins ? 1 : 0;
}

bool vstamp_same(const vstamp *a, const vstamp *b) {
  return (bool)(a->ts == b->ts && a->coord == b->coord &&
                vclock_compare(&a->clock, &b->clock) == VCLOCK_EQUAL);
}

size_t vclock_encode(const vclock *c, char out[VCLOCK_TEXT_MAX]) {
  size_t len = 0;
  out[0] = '\0';
  for (size_t i = 0; i != c->len; i++) {
    int n = snprintf(out + len, VCLOCK_TEXT_MAX - len, "%s%d:%" PRIu64 ":%" PRIu64, i ? "," : "",
                     c->e[i].id, c->e[i].n, c->e[i].ts);
    len += n > 0 ? (size_t)n : 0;
  }
  return len;
}

size_t vclock_parse_u64(const char *text, size_t len, uint64_t *out) {
  uint64_t v = 0;
  size_t i = 0;
  for (; i != len && text[i] >= '0' && text[i] <= '9'; i++) {
    unsigned digit = (unsigned)(text[i] - '0');
    if (v > (UINT64_MAX - digit) / 10) {
      return 0;
    }
    v = (v * 10) + digit;
  }
  *out = v;
  return i;
}

/** Parse the decimal at *p, before end, into *out and move *p past it. Returns 0 or -1. */
static int parse_u64(const char **p, const char *end, uint64_t *out) {
  size_t n = vclock_parse_u64(*p, (size_t)(end - *p), out);
  *p += n;
  return n ? 0 : -1;
}

/** Parse "id:n:ts" at *p into *e and move *p past it. Returns 0 or -1. */
static int parse_entry(const char **p, const char *end, vclock_entry *e) {
  uint64_t id = 0;
  if (parse_u64(p, end, &id) != 0 || *p == end || **p != ':') {
    return -1;
  }
  (*p)++;
  if (parse_u64(p, end, &e->n) != 0 || id == 0 || id > INT32_MAX || e->n == 0 || *p == end ||
      **p != ':') {
    return -1;
  }
  (*p)++;
  if (parse_u64(p, end, &e->ts) != 0) {
    return -1;
  }
  e->id = (int)id;
  return 0;
}

int vclock_decode(const char *text, size_t len, vclock *out) {
  out->len = 0;
  const char *p = text;
  const char *end = text + len;
  while (p != end) {
    if (out->len != 0) {
      if (*p != ',') {
        return -1;
      }
      p++;
    }
    vclock_entry e;
    if (out->len == VCLOCK_MAX || parse_entry(&p, end, &e) != 0 ||
        (out->len != 0 && e.id <= out->e[out->len - 1].id)) {
      return -1;
    }
    out->e[out->len++] = e;
  }
  return 0;
}
