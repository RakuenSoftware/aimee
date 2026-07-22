/* roundtable_pipeline_chunk.h: split a large review artifact into budget-sized
 * chunks and build the whole-artifact synthesis assembly manifest (proposal
 * sections 2/3, #28/#32/#34/#37/#39).
 *
 * The design invariant: chunks are DERIVED from the retained whole origin, never
 * a separate durable store that can leak or go stale. Re-deriving from the
 * origin each pass gives per-pass freshness (#42) and parked-gate release (#47)
 * for free, and the origin is always recoverable (#34). */
#ifndef DEC_ROUNDTABLE_PIPELINE_CHUNK_H
#define DEC_ROUNDTABLE_PIPELINE_CHUNK_H 1

#ifdef __cplusplus
extern "C"
{
#endif

#define RTP_MAX_CHUNKS     64
#define RTP_CHUNK_HASH_LEN 17 /* 16 hex + NUL */

   typedef struct
   {
      int index;
      int offset; /* byte offset into the origin */
      int len;    /* byte length of this chunk */
      char hash[RTP_CHUNK_HASH_LEN];
   } rtp_chunk_t;

   typedef struct
   {
      rtp_chunk_t chunks[RTP_MAX_CHUNKS];
      int count;
      int budget_bytes; /* per-chunk maximum (net of reserved headroom) */
      int origin_len;
      char origin_hash[RTP_CHUNK_HASH_LEN];
      int over_budget; /* an indivisible single line exceeded the budget */
      int truncated;   /* origin needed more than RTP_MAX_CHUNKS chunks */
   } rtp_chunk_plan_t;

   /* True iff `origin` (origin_len bytes) needs chunking for budget_bytes. */
   int rtp_chunk_needed(const char *origin, int budget_bytes);

   /* Split `origin` into chunks each <= budget_bytes, preferring line
    * boundaries. Computes a per-chunk content hash and a whole-origin hash.
    * Returns 0 on success; sets out->over_budget / out->truncated on the
    * degenerate cases (still returns 0 so the caller can surface a coverage
    * gap rather than crash). budget_bytes <= 0 -> a single chunk. */
   int rtp_chunk_plan(const char *origin, int budget_bytes, rtp_chunk_plan_t *out);

   /* Re-verify that every chunk in `plan` still hashes back to the current
    * `origin` (no stale span served from an earlier revision, #34/#42).
    * Returns 1 if all match, 0 otherwise. */
   int rtp_chunk_verify(const char *origin, const rtp_chunk_plan_t *plan);

   /* The whole-artifact synthesis assembly (#39): greedily select chunk spans
    * (by index) whose cumulative size fits budget_bytes; the remainder are
    * omitted and recorded as a coverage gap. A single chunk larger than the
    * budget sets over_budget. */
   typedef struct
   {
      int selected[RTP_MAX_CHUNKS];
      int selected_count;
      int omitted[RTP_MAX_CHUNKS];
      int omitted_count;
      int budget_bytes;
      int used_bytes;
      int over_budget;
   } rtp_assembly_t;

   int rtp_assembly_build(const rtp_chunk_plan_t *plan, int budget_bytes, rtp_assembly_t *out);

   /* Stable 64-bit FNV-1a over n bytes, lowercase hex (16 chars + NUL). Shared
    * so chunk hashes and origin hashes use one implementation. */
   void rtp_chunk_hash(const char *data, int n, char *out, int out_cap);

#ifdef __cplusplus
}
#endif

#endif /* DEC_ROUNDTABLE_PIPELINE_CHUNK_H */
