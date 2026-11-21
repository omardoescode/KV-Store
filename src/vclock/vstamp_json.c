#include "vclock/vstamp_json.h"

bool vstamp_from_json(const json_val *obj, vstamp *out) {
  size_t clock_len = 0;
  const char *clock = json_str(json_get(obj, "clock"), &clock_len);
  const json_val *ts = json_get(obj, "ts");
  const json_val *coord = json_get(obj, "coord");
  if (!clock || vclock_decode(clock, clock_len, &out->clock) != 0 ||
      json_type_of(ts) != JSON_NUMBER || json_int(ts) < 0 || json_type_of(coord) != JSON_NUMBER ||
      json_int(coord) < 0 || json_int(coord) > INT32_MAX) {
    return false;
  }
  out->ts = (uint64_t)json_int(ts);
  out->coord = (int)json_int(coord);
  return true;
}

bool vstamp_to_json(json_builder *b, json_node *obj, const vstamp *s) {
  char clock[VCLOCK_TEXT_MAX];
  size_t clock_len = vclock_encode(&s->clock, clock);
  return (bool)(json_put(b, obj, "clock", json_new_str(b, clock, clock_len)) &&
                json_put(b, obj, "ts", json_new_int(b, (int64_t)s->ts)) &&
                json_put(b, obj, "coord", json_new_int(b, s->coord)));
}
