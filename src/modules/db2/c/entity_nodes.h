/* src/modules/db2/c/entity_nodes.h: entity_nodes table + aliases — Postgres via libpq. */

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
} db2_entity_node_t;

/* Percent-encode a single component using the proposal's encoding rules.
 * Unescaped set: A-Za-z0-9._-~/. Literal ':', '%', spaces, control bytes,
 * and non-ASCII are escaped as %XX with uppercase hex.
 * out must be at least 3*strlen(in)+1 bytes. Returns bytes written or -1. */
int db2_entity_node_encode_component(const char *in, char *out, size_t cap);

/* Build canonical keys. out must be GRAPH_ENDPOINT_MAX bytes.
 * Returns 0 on success, -1 on invalid input. */
int db2_entity_node_key_file(const char *project, const char *path, char *out, size_t cap);
int db2_entity_node_key_symbol(const char *project, const char *name, char *out, size_t cap);
int db2_entity_node_key_concept(const char *token, char *out, size_t cap);
int db2_entity_node_key_project(const char *name, char *out, size_t cap);

/* Upsert an entity_nodes row. Returns 0 on success, -1 on DB error. */
int db2_entity_node_upsert(const char *node_key, int node_kind, const char *project,
                           const char *display_name, const char *full_key, const char *file_path,
                           const char *symbol, const char *node_origin, int64_t generation_id);

/* Look up a node by key. Returns 0 on success, -1 if not found or error. */
int db2_entity_node_get(const char *node_key, db2_entity_node_t *out);

/* Upsert an alias row. Returns 0 on success, -1 on error. */
int db2_entity_node_alias_upsert(const char *alias, const char *node_key, const char *alias_kind,
                                 const char *project, int64_t generation_id);

/* Project-scoped alias resolution. Writes up to max canonical node_keys
 * for the given alias within the project. Returns count. */
int db2_entity_node_resolve_alias(const char *alias, const char *project,
                                  char (*out_keys)[GRAPH_ENDPOINT_MAX], int max);

/* Stale cleanup: delete entity_nodes with node_origin='code_projection' and
 * last_seen_generation_id < min_generation_id. Returns deleted count. */
int db2_entity_node_cleanup_stale_code(int64_t min_generation_id);

/* Stale cleanup: delete entity_node_aliases with last_seen_generation_id <
 * min_generation_id for the given project (or all projects if NULL).
 * Returns deleted count. */
int db2_entity_node_alias_cleanup_stale(const char *project, int64_t min_generation_id);

#endif /* ENTITY_NODES_H */
