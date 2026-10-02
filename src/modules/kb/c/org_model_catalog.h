/* kb_store/org_model_catalog.h: P2a org model catalog + entitlement (tiered-llm-p2a).
 *
 * Thin C access layer over the SECURITY DEFINER catalog functions in kb_store/schema.sql
 * (org_catalog_entitled, org_catalog_upsert, org_catalog_remove, org_model_entitle,
 * org_model_unentitle). Tenant-scoped: every entry requires the RLS-enforcing Postgres
 * backend (kb_store_tenant_require_pg) — the SQLite shim carries the columns only. Writes go
 * ONLY through the audited definer functions (never a raw INSERT), so both the HTTP
 * routes and the CLI are WORM-audited by one code path. Catalog-only: holds NO keys,
 * no egress, no credential/slot reference. kb-only (rides KB_STORE_SRCS -> KB_KB_STORE_OBJS). */
#ifndef DEC_KB_STORE_ORG_MODEL_CATALOG_H
#define DEC_KB_STORE_ORG_MODEL_CATALOG_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Admin-denied return code (the SECURITY DEFINER function RAISEd SQLSTATE 42501 —
 * "admin only"). Distinct from a generic -1 so a mutation handler can map an admin
 * denial to HTTP 403 and every other DB/constraint/audit failure to HTTP 500. */
#define KB_STORE_MODEL_ERR_DENIED (-2)

/* Catalog endpoint buffer. Sized so a stored endpoint (schema CHECK: <=500 chars)
 * round-trips through the HTTP read, the struct field, and the read-back with NO
 * silent truncation. One size, used end-to-end. */
#define KB_STORE_MODEL_ENDPOINT_CAP 512
#define KB_STORE_BEDROCK_ARRAY_MAX  64
#define KB_STORE_BEDROCK_REGION_CAP 256
#define KB_STORE_BEDROCK_ARN_CAP    512

   typedef enum
   {
      KB_STORE_BEDROCK_TARGET_OK = 0,
      KB_STORE_BEDROCK_TARGET_UNAVAILABLE,
      KB_STORE_BEDROCK_TARGET_INVALID,
      KB_STORE_BEDROCK_TARGET_ERROR
   } kb_store_bedrock_target_result_t;

   typedef struct
   {
      char model_id[208];
      char bedrock_api[16];
      char model_family[112];
      char target_type[48];
      char partition[16];
      char account[16];
      char invoke_region[KB_STORE_BEDROCK_REGION_CAP];
      char endpoint[KB_STORE_MODEL_ENDPOINT_CAP];
      char regions[KB_STORE_BEDROCK_ARRAY_MAX][KB_STORE_BEDROCK_REGION_CAP];
      size_t n_regions;
      char underlying_fm_arns[KB_STORE_BEDROCK_ARRAY_MAX][KB_STORE_BEDROCK_ARN_CAP];
      size_t n_underlying;
   } kb_store_bedrock_target_t;

   /* Raw PostgreSQL adapter row. Exposed so the mandatory hostile-row unit can exercise
    * malformed values PostgreSQL TEXT/JSON itself cannot produce. */
   typedef struct
   {
      const char *model_id;
      const char *bedrock_api;
      const char *model_family;
      const char *target_type;
      const char *partition;
      const char *account;
      const char *invoke_region;
      const char *regions_json;
      const char *underlying_json;
      const char *endpoint;
   } kb_store_bedrock_target_row_t;

   kb_store_bedrock_target_result_t
   kb_store_model_bedrock_target_decode_row(const kb_store_bedrock_target_row_t *row,
                                            kb_store_bedrock_target_t *out);

   kb_store_bedrock_target_result_t
   kb_store_model_bedrock_target_resolve(int64_t team_id, const char *model_id,
                                         kb_store_bedrock_target_t *out);

   /* One entitled-model row as returned by org_catalog_entitled(). NO credential or
    * slot field — the entitled surface is the authoritative catalog columns only. */
   typedef struct
   {
      char model_id[208];
      char display_name[256];
      char provider[112];
      char wire[16];
      char endpoint[KB_STORE_MODEL_ENDPOINT_CAP];
   } kb_store_model_entitled_row_t;

   /* One full catalog row (admin view — includes enabled + the endpoint). */
   typedef struct
   {
      char model_id[208];
      char display_name[256];
      char provider[112];
      char wire[16];
      char endpoint[KB_STORE_MODEL_ENDPOINT_CAP];
      int enabled;
   } kb_store_model_catalog_row_t;

   /* The full catalog, ordered by model_id (admin/operator read — RLS admits it only for
    * an org-admin principal). Fills out[0..n) and returns n (>=0), or negative on error.
    * Must run inside an open tenant scope with an admin principal. */
   int kb_store_model_catalog_list(kb_store_model_catalog_row_t *out, int max);

   /* The current actor's entitled models (org_catalog_entitled(), actor-bound to
    * aimee.principal). Fills out[0..n) and returns n (>=0), or a negative kb_store/tenant
    * error. Must run inside an open tenant scope (kb_store_tenant_scope_begin). */
   int kb_store_model_entitled_list(kb_store_model_entitled_row_t *out, int max);

   /* Admin-gated create-or-update of a catalog row (org_catalog_upsert). Returns 0 on
    * success (id in *out_id when non-NULL), non-zero on denial/error. WORM-audited. */
   int kb_store_model_catalog_upsert(const char *model_id, const char *display_name,
                                     const char *provider, const char *wire, const char *endpoint,
                                     int enabled, int64_t *out_id);

   /* Admin-gated remove of a catalog row + its entitlements (org_catalog_remove).
    * *out_removed (when non-NULL) gets the catalog rows removed (0 = unknown model). */
   int kb_store_model_catalog_remove(const char *model_id, int64_t *out_removed);

   /* Admin-gated grant / revoke of (model, team) (org_model_entitle / _unentitle). */
   int kb_store_model_entitle(const char *model_id, int64_t team_id, int64_t *out_id);
   int kb_store_model_unentitle(const char *model_id, int64_t team_id, int64_t *out_removed);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_ORG_MODEL_CATALOG_H */
