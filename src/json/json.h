#ifndef JSON_JSON_H
#define JSON_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Read side: a parsed document and the values in it. Values belong to their document.
typedef struct json_doc json_doc;
typedef struct json_val json_val;

// Write side: a builder and the nodes it makes. Nodes belong to their builder.
typedef struct json_builder json_builder;
typedef struct json_node json_node;

typedef enum {
  JSON_NONE,
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT,
} json_type;

/** Parse len bytes of JSON; return NULL if they are not valid JSON. Free with json_doc_free. */
json_doc *json_parse(const char *data, size_t len);
/** Free a document and all its values; NULL is permitted. */
void json_doc_free(json_doc *doc);
/** Return the top value of the document. */
const json_val *json_doc_root(const json_doc *doc);

// All read functions accept NULL and values of the wrong type: they return the "absent" result
// (JSON_NONE, NULL, 0, false). Thus calls can be chained: json_get(json_get(root, "a"), "b").

/** Return the type of v, or JSON_NONE if v is NULL. */
json_type json_type_of(const json_val *v);
/** Return the value of key in an object, or NULL if absent. Time is linear in the object size. */
const json_val *json_get(const json_val *obj, const char *key);
/** Return element index of an array, or NULL if out of range. Time is linear in index. */
const json_val *json_at(const json_val *arr, size_t index);
/** Return the element count of an array, the pair count of an object, or the string length. */
size_t json_len(const json_val *v);
/** Return the value of a boolean; false if v is NULL or not a boolean. */
bool json_bool(const json_val *v);
/** Return the value of an integer; 0 if v is NULL or not an integer. */
int64_t json_int(const json_val *v);
/** Return the value of a number of any kind as double; 0 if v is NULL or not a number. */
double json_num(const json_val *v);
/**
 * Return the borrowed NUL-terminated string, valid until json_doc_free, and set *len if len is
 * not NULL. Return NULL and set *len to 0 if v is NULL or not a string.
 */
const char *json_str(const json_val *v, size_t *len);

/** Allocate an empty builder; return NULL on failure. Free with json_builder_free. */
json_builder *json_builder_new(void);
/** Free a builder and all its nodes; NULL is permitted. */
void json_builder_free(json_builder *b);

// All json_new_* functions return a node owned by b, or NULL on allocation failure.

/** Make an empty object node. */
json_node *json_new_obj(json_builder *b);
/** Make an empty array node. */
json_node *json_new_arr(json_builder *b);
/** Make a string node from a copy of len bytes of s. */
json_node *json_new_str(json_builder *b, const char *s, size_t len);
/** Make an integer node. */
json_node *json_new_int(json_builder *b, int64_t value);
/** Make a floating-point node. */
json_node *json_new_num(json_builder *b, double value);
/** Make a boolean node. */
json_node *json_new_bool(json_builder *b, bool value);
/** Make a null node. */
json_node *json_new_null(json_builder *b);

/**
 * Add key: val to obj, copying the NUL-terminated key. Does not check for an existing key, so
 * duplicate keys are possible.
 * Return false if an argument is NULL, obj is not an object, or allocation fails.
 */
bool json_put(json_builder *b, json_node *obj, const char *key, json_node *val);
/** Append val to arr; return false if an argument is NULL or arr is not an array. */
bool json_push(json_node *arr, json_node *val);

/**
 * Serialize root as compact JSON into a new malloc buffer and set *len to its length.
 * The caller must free() the result. Return NULL and set *len to 0 on failure.
 */
char *json_to_bytes(const json_node *root, size_t *len);

#endif
