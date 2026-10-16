#include "json/json.h"

#include "yyjson.h"

/** Parse len bytes of JSON with the default allocator; return NULL if they are not valid. */
json_doc *json_parse(const char *data, size_t len) { return (json_doc *)yyjson_read(data, len, 0); }

/** Free a parsed document and all values in it; NULL is permitted. */
void json_doc_free(json_doc *doc) { yyjson_doc_free((yyjson_doc *)doc); }

/** Return the borrowed top value of doc, or NULL if doc is NULL. */
const json_val *json_doc_root(const json_doc *doc) {
  return (const json_val *)yyjson_doc_get_root((yyjson_doc *)doc);
}

/** Map the type of v to json_type; return JSON_NONE if v is NULL. */
json_type json_type_of(const json_val *v) {
  switch (yyjson_get_type((yyjson_val *)v)) {
  case YYJSON_TYPE_NULL:
    return JSON_NULL;
  case YYJSON_TYPE_BOOL:
    return JSON_BOOL;
  case YYJSON_TYPE_NUM:
    return JSON_NUMBER;
  case YYJSON_TYPE_STR:
    return JSON_STRING;
  case YYJSON_TYPE_ARR:
    return JSON_ARRAY;
  case YYJSON_TYPE_OBJ:
    return JSON_OBJECT;
  default:
    return JSON_NONE;
  }
}

/** Return the borrowed value of key in obj, or NULL if absent or obj is not an object. */
const json_val *json_get(const json_val *obj, const char *key) {
  return (const json_val *)yyjson_obj_get((yyjson_val *)obj, key);
}

/** Return the borrowed element index of arr, or NULL if out of range or arr is not an array. */
const json_val *json_at(const json_val *arr, size_t index) {
  return (const json_val *)yyjson_arr_get((yyjson_val *)arr, index);
}

/** Return the element, pair, or byte count of an array, object, or string; 0 otherwise. */
size_t json_len(const json_val *v) { return yyjson_get_len((yyjson_val *)v); }

/** Return the value of a boolean; false if v is NULL or not a boolean. */
bool json_bool(const json_val *v) { return yyjson_get_bool((yyjson_val *)v); }

/** Return the value of an integer; 0 if v is NULL or not an integer. */
int64_t json_int(const json_val *v) { return yyjson_get_sint((yyjson_val *)v); }

/** Return the value of a number of any kind as double; 0 if v is NULL or not a number. */
double json_num(const json_val *v) { return yyjson_get_num((yyjson_val *)v); }

/**
 * Return the borrowed NUL-terminated string, valid until json_doc_free, and set *len if len is
 * not NULL. Return NULL and set *len to 0 if v is NULL or not a string.
 */
const char *json_str(const json_val *v, size_t *len) {
  if (len != NULL) {
    *len = yyjson_is_str((yyjson_val *)v) ? yyjson_get_len((yyjson_val *)v) : 0;
  }
  return yyjson_get_str((yyjson_val *)v);
}

/** Allocate an empty builder; return NULL on allocation failure. */
json_builder *json_builder_new(void) { return (json_builder *)yyjson_mut_doc_new(NULL); }

/** Free a builder and all nodes made with it; NULL is permitted. */
void json_builder_free(json_builder *b) { yyjson_mut_doc_free((yyjson_mut_doc *)b); }

/** Make an empty object node owned by b; return NULL on allocation failure. */
json_node *json_new_obj(json_builder *b) {
  return (json_node *)yyjson_mut_obj((yyjson_mut_doc *)b);
}

/** Make an empty array node owned by b; return NULL on allocation failure. */
json_node *json_new_arr(json_builder *b) {
  return (json_node *)yyjson_mut_arr((yyjson_mut_doc *)b);
}

/** Make a string node owned by b from a copy of len bytes of s; return NULL on failure. */
json_node *json_new_str(json_builder *b, const char *s, size_t len) {
  return (json_node *)yyjson_mut_strncpy((yyjson_mut_doc *)b, s, len);
}

/** Make an integer node owned by b; return NULL on allocation failure. */
json_node *json_new_int(json_builder *b, int64_t value) {
  return (json_node *)yyjson_mut_sint((yyjson_mut_doc *)b, value);
}

/** Make a floating-point node owned by b; return NULL on allocation failure. */
json_node *json_new_num(json_builder *b, double value) {
  return (json_node *)yyjson_mut_real((yyjson_mut_doc *)b, value);
}

/** Make a boolean node owned by b; return NULL on allocation failure. */
json_node *json_new_bool(json_builder *b, bool value) {
  return (json_node *)yyjson_mut_bool((yyjson_mut_doc *)b, value);
}

/** Make a null node owned by b; return NULL on allocation failure. */
json_node *json_new_null(json_builder *b) {
  return (json_node *)yyjson_mut_null((yyjson_mut_doc *)b);
}

/**
 * Add key: val to obj, copying the NUL-terminated key into b. Does not check for an existing
 * key, so duplicate keys are possible.
 * Return false if an argument is NULL, obj is not an object, or allocation fails.
 */
bool json_put(json_builder *b, json_node *obj, const char *key, json_node *val) {
  yyjson_mut_val *k = yyjson_mut_strcpy((yyjson_mut_doc *)b, key);
  return yyjson_mut_obj_add((yyjson_mut_val *)obj, k, (yyjson_mut_val *)val);
}

/** Append val to arr; return false if an argument is NULL or arr is not an array. */
bool json_push(json_node *arr, json_node *val) {
  return yyjson_mut_arr_append((yyjson_mut_val *)arr, (yyjson_mut_val *)val);
}

/**
 * Serialize root as compact JSON into a new malloc buffer and set *len to its length.
 * The caller must free() the result. Return NULL and set *len to 0 on failure.
 */
char *json_to_bytes(const json_node *root, size_t *len) {
  char *bytes = yyjson_mut_val_write((const yyjson_mut_val *)root, 0, len);
  if (bytes == NULL && len != NULL) {
    *len = 0;
  }
  return bytes;
}
