/* kb_curator_synthesize.h: deep-curator synthesize_topic pass.
 *
 * Periodically picks a high-centrality topic (the committed entity with the
 * most inbound `mentions` that has no synthesis written about it yet), gathers
 * the top-K artifacts that mention it, and asks the curator_synthesize sidecar
 * to emit a `synthesis` narrative artifact with citations. The synthesis is
 * linked `about` the topic entity and `cites` its sources so a second
 * synthesize pass over the same entity is suppressed.
 *
 * Off by default: requires kb.curator.synthesize.enabled AND a configured
 * kb.curator.synthesize_command. No DB1 access. */
#ifndef KB_CURATOR_SYNTHESIZE_H
#define KB_CURATOR_SYNTHESIZE_H

#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Process one topic: pick -> gather -> synthesize -> write artifact + links.
 * Returns 1 if a synthesis was written, 0 if nothing was eligible (or no DB /
 * sidecar disabled), so the drain can rate-limit. */
#include <stddef.h>
#include <stdint.h>

/* Test seam: pick the highest-centrality un-synthesised topic — the committed
 * entity with the most inbound `mentions` and no `synthesis` linked `about` it.
 * Returns 1 + fills out params on a hit (*payload_out heap-allocated, caller
 * frees), else 0. */
int kb_curator_synth_pick_topic(void *conn, char *id, size_t id_len, char **payload_out,
                                char *scope_kind, size_t sk_len, char *scope_id, size_t si_len);

int kb_curator_restore_fragment_record(int64_t fragment_doc_id, const char *base_artifact_id,
                                       const char *restored_text, double confidence,
                                       const char *prompt_version, char *artifact_id_out,
                                       size_t artifact_id_len);

int kb_curator_synthesize_one(const kb_curator_extract_opts_t *opts);

#endif /* KB_CURATOR_SYNTHESIZE_H */
