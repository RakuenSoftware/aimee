/* embedder_catalog.h — the selectable-embedder list behind GET /v1/embedders, read from
 * the embedder registry (scripts/embedders.json) rather than from a running gateway: the
 * setup wizard needs the list before anything is deployed. */
#ifndef DEC_EMBEDDER_CATALOG_H
#define DEC_EMBEDDER_CATALOG_H 1

#include <stddef.h>

typedef struct cJSON cJSON;

/* Read the registry file (AIMEE_EMBEDDERS_FILE, then /opt/aimee/embedders.json, then
 * scripts/embedders.json). Returns a malloc'd NUL-terminated buffer the caller frees, or
 * NULL when no candidate path is readable. */
char *embedder_registry_read(void);

/* Shape the registry into the wizard's array: one object per embedder carrying id, dim,
 * context, pooling, local and prefixed. Every field is included because every one
 * of them changes the vectors, so a picker that hid them would invite a silent re-embed.
 * Returns a new cJSON array the caller owns, or NULL with *err set (a static string). */
cJSON *embedder_catalog_build(const char *raw, const char **err);

/* Whether the supervisor can fetch this entry's weights itself, which is what decides
 * whether it is the bundled option. Needs a repo at a pinned revision. */
int embedder_entry_is_local(cJSON *entry);

#endif /* DEC_EMBEDDER_CATALOG_H */
