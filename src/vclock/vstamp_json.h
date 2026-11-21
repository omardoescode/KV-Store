#ifndef VCLOCK_VSTAMP_JSON_H
#define VCLOCK_VSTAMP_JSON_H

#include <stdbool.h>

#include "vclock/vclock.h"
#include "json/json.h"

/**
 * Read the "clock" (vclock text), "ts" (uint) and "coord" (int32 >= 0) of the JSON object obj into
 * *out. Returns false if one is missing or malformed.
 */
bool vstamp_from_json(const json_val *obj, vstamp *out);

/** Add "clock", "ts" and "coord" of s to the JSON object obj. Returns false on error. */
bool vstamp_to_json(json_builder *b, json_node *obj, const vstamp *s);

#endif
