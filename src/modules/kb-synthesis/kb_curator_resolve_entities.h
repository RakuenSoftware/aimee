/* kb_curator_resolve_entities.h: deep-curator resolve_entities pass.
 *
 * Turns proposed `entity` mention artifacts (emitted by extract_doc) into
 * committed canonical entities and populates curator_entity_vectors. This first
 * chunk does embed + commit + upsert; NN-search dedup/merge lands separately.
 * No DB1 access. */
#ifndef KB_CURATOR_RESOLVE_ENTITIES_H
#define KB_CURATOR_RESOLVE_ENTITIES_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

#include <stddef.h>
#include <stdint.h>

/* Process one proposed `entity` mention: embed its name+context, upsert into
 * curator_entity_vectors, and commit the artifact. Returns 1 if one mention was
 * processed, 0 if none were pending (or no DB), so the drain can rate-limit. */
int kb_curator_resolve_entities_one(const kb_curator_extract_opts_t *opts);

/* Pure helpers (exposed for unit tests). */

/* Compose the text embedded for an entity: "<name> — <context>" (name alone
 * when context is empty; "" when name is empty). Always NUL-terminates. */
void kb_curator_entity_embed_text(const char *name, const char *context, char *out, size_t out_len);

/* Deterministic, positive 64-bit point id for an entity artifact (FNV-1a over
 * the artifact id string). Used as the curator_entity_vectors primary key. */
int64_t kb_curator_entity_point_id(const char *artifact_id);

#endif /* KB_CURATOR_RESOLVE_ENTITIES_H */
