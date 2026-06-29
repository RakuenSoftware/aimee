/* episode_seal.h: sealed work episode — file inventory + conclusion (fold §5, P5).
 *
 * Unlike the existing narrative episode cards (KIND_EPISODE), a *sealed* episode
 * is a replayable checkpoint: the set of files touched plus the agent's
 * conclusion, so a later session that re-touches a member file can auto-recall
 * what was learned. Stored as a distinct unit_type (EPISODE_SEAL_UNIT_TYPE) on the
 * existing memory_units row — no schema column, no migration. Pure: cJSON only.
 *
 * This slice ships the seal record + serialize/parse + the file-touch matcher
 * (the auto-recall predicate). The DB2 store/query + cross-session recall wiring
 * is the storage-binding follow-up. */
#ifndef DEC_EPISODE_SEAL_H
#define DEC_EPISODE_SEAL_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Distinct memory_units.unit_type value for sealed episodes (vs the narrative
 * "episode_card"); avoids overloading KIND_EPISODE. */
#define EPISODE_SEAL_UNIT_TYPE "episode_seal"

   typedef struct
   {
      char *conclusion; /* what was concluded/settled */
      char **files;     /* file inventory (paths touched) */
      size_t count;
      size_t cap;
   } episode_seal_t;

   void episode_seal_init(episode_seal_t *s);
   void episode_seal_free(episode_seal_t *s);

   /* Set/replace the conclusion text. Returns 0, -1 on alloc failure. */
   int episode_seal_set_conclusion(episode_seal_t *s, const char *text);

   /* Add a file path to the inventory (dedup, exact). NULL/empty is ignored (a
    * no-op success). Returns 0 on success (incl. ignored/dup), -1 on alloc failure. */
   int episode_seal_add_file(episode_seal_t *s, const char *path);

   /* Auto-recall predicate: 1 if `path` is in the seal's file inventory (exact
    * match), i.e. a later turn touching this file should recall this episode. */
   int episode_seal_touches(const episode_seal_t *s, const char *path);

   /* Serialize to a freshly malloc'd JSON string (caller frees). Deterministic. */
   char *episode_seal_serialize(const episode_seal_t *s);

   /* Restore from JSON. Validates and builds into a temporary, swapping into *s
    * only on complete success — so on -1 (malformed JSON, non-object root,
    * non-array files, or OOM) the caller's existing seal is left UNCHANGED. */
   int episode_seal_parse(episode_seal_t *s, const char *json);

#ifdef __cplusplus
}
#endif

#endif /* DEC_EPISODE_SEAL_H */
