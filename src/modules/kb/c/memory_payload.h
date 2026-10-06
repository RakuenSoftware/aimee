/* memory_payload.h: KB_STORE domain helpers that build vector point payloads.
 *
 * The payload JSON is pure KB_STORE data — memories, memory_units,
 * memory_scopes, memory_entities rows assembled for the point body.
 * The pgvector upsert path calls these builders immediately before
 * writing.
 *
 * Pure domain API.  Backend access stays private to src/modules/kb/c/.
 */
#ifndef DEC_KB_STORE_MEMORY_PAYLOAD_H
#define DEC_KB_STORE_MEMORY_PAYLOAD_H 1

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Legacy wire-catalog declaration only. No native implementation or
    * production caller remains; Go memory owns exact-key lookup. Retire this
    * declaration with the older generated KB_STORE key_exists wire contract. */
   int kb_store_memory_key_exists(const char *key);

   /* Total row count in the memories table. Returns 0 on error. */
   int64_t kb_store_memory_count(void);

   /* Auditable-correctness P2 (/v1/audit/provenance): resolve a surfaced memory
    * id to its provenance fields — kind, source (source_session), and version
    * (the row's updated_at). Any out buffer may be NULL. Returns 1 on hit, 0 when
    * no such row exists (deleted/superseded since the turn — itself a provenance
    * signal), -1 on error. */

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_MEMORY_PAYLOAD_H */
