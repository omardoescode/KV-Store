#ifndef KV_HANDLERS_H
#define KV_HANDLERS_H

#include "http/http.h"
#include "kv_store.h"

/**
 * Register GET/PUT/DELETE handlers for /kv/<key> on server, backed by store.
 */
void kv_register_routes(http_server *server, kv_store *store);

#endif /* KV_HANDLERS_H */