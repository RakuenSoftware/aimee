/* src/modules/kb/c/entity_nodes.h: entity_nodes table + aliases — Postgres via libpq. */

#ifndef ENTITY_NODES_H
#define ENTITY_NODES_H

#include <stddef.h>
#include <stdint.h>

/* GRAPH_ENDPOINT_MAX fallback — mirrors headers/memory.h */
#ifndef GRAPH_ENDPOINT_MAX
#define GRAPH_ENDPOINT_MAX 512
#endif

/* Node origin vocabulary (null-terminated constants). */
#define ENTITY_NODE_ORIGIN_CODE_PROJECTION   "code_projection"
#define ENTITY_NODE_ORIGIN_MEMORY_EXTRACTION "memory_extraction"
#define ENTITY_NODE_ORIGIN_SESSION           "session"
#define ENTITY_NODE_ORIGIN_CONCEPT           "concept"
#define ENTITY_NODE_ORIGIN_MANUAL            "manual"

/* Struct for an entity_nodes row result. */
typedef struct
{
   char node_key[GRAPH_ENDPOINT_MAX];
   int node_kind; /* memory_node_kind_t int code */
   char project[256];
   char display_name[256];
   char full_key[GRAPH_ENDPOINT_MAX];
   char file_path[512];
   char symbol[256];
   char node_origin[32];
   int64_t last_seen_generation_id;
} kb_store_entity_node_t;

/* Percent-encode a single component using the proposal's encoding rules.
 * Unescaped set: A-Za-z0-9._-~/. Literal ':', '%', spaces, control bytes,
 * and non-ASCII are escaped as %XX with uppercase hex.
 * out must be at least 3*strlen(in)+1 bytes. Returns bytes written or -1. */
int kb_store_entity_node_encode_component(const char *in, char *out, size_t cap);

/* Build canonical keys. out must be GRAPH_ENDPOINT_MAX bytes.
 * Returns 0 on success, -1 on invalid input. */
int kb_store_entity_node_key_file(const char *project, const char *path, char *out, size_t cap);
int kb_store_entity_node_key_symbol(const char *project, const char *name, char *out, size_t cap);
int kb_store_entity_node_key_concept(const char *token, char *out, size_t cap);
int kb_store_entity_node_key_project(const char *name, char *out, size_t cap);

/* Look up a node by key. Returns 0 on success, -1 if not found or error. */
int kb_store_entity_node_get(const char *node_key, kb_store_entity_node_t *out);

#endif /* ENTITY_NODES_H */
