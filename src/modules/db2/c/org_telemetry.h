/* db2/org_telemetry.h: P9a kb telemetry export + content-free ingest target
 * (tiered-llm-p9a-telemetry-export).
 *
 * Thin typed C access over the SECURITY DEFINER telemetry functions in
 * db2/schema.sql (org_telemetry_ingest / org_telemetry_allow[_show] /
 * org_metrics_snapshot). Two independent surfaces: /v1/metrics renders the
 * AUTHORITATIVE-STATE aggregate (org_metrics_snapshot — never org_telemetry), and
 * the content-free allowlist-gated ingest is the target the deferred forwarder
 * (P9 §1) will call. The definer enforces admin authz (allow/show/snapshot) and
 * the fail-closed drop-on-unknown (ingest) INTERNALLY; this layer maps the
 * admin-gate RAISE to a sentinel (42501 -> DENIED). Tenant-scoped: requires the
 * RLS-enforcing Postgres backend. kb-only (rides DB2_SRCS -> KB_DB2_OBJS; never
 * enters aimee-server). */
#ifndef DEC_DB2_ORG_TELEMETRY_H
#define DEC_DB2_ORG_TELEMETRY_H 1

#include <stdint.h>

#include "org_telemetry_fmt.h"

#ifdef __cplusplus
extern "C"
{
#endif

/* The definer RAISEd SQLSTATE 42501 (an admin-gated allow/show/snapshot by a
 * non-admin). Distinct from -1 so the HTTP route maps it to 403 and every other
 * failure to 500. */
#define DB2_TELEMETRY_ERR_DENIED (-2)

/* The metrics snapshot produced more series than the caller's buffer holds. A
 * silent truncation would return a successful-but-incomplete Prometheus export
 * (missing series for a very large org), so the snapshot fails LOUDLY instead —
 * the HTTP route maps it to 500. (Mirrors the P3b spend-report TOOBIG posture.) */
#define DB2_TELEMETRY_ERR_TOOBIG (-3)

/* Ingest outcome strings (as returned by org_telemetry_ingest). */
#define DB2_TELEMETRY_RESULT_MAX 16

/* Max rows a metrics snapshot / allowlist show may carry (bounded aggregates). */
#define DB2_TELEMETRY_MAX_ROWS       4096
#define DB2_TELEMETRY_ALLOW_MAX_ROWS 256

   /* One allowlist row, as returned by org_telemetry_allow_show(). metric_names is
    * the Postgres array TEXT (e.g. '{a,b}') — rendered as-is for the operator. */
   typedef struct
   {
      char event_schema[160];
      char metric_names[1024]; /* the '{...}' array literal */
      int enabled;
      char updated_at[40];
   } db2_telemetry_allow_row_t;

   /* Content-free allowlist-gated ingest (org_telemetry_ingest). origin_cn is the
    * SERVER-SET caller identity (never body text); team < 0 => NULL team_id. value
    * is decimal text (bound as NUMERIC). Writes the outcome ('stored'|'deduped'|
    * 'dropped') into out_result[cap]. Returns 0 on a DB-level outcome, -1 on error.
    * Runs inside an open tenant scope. */
   int db2_telemetry_ingest(const char *source_event_id, const char *origin_cn, int has_team,
                            int64_t team, const char *event_schema, const char *metric_name,
                            const char *metric_kind, const char *value_text, int64_t ts,
                            char *out_result, int cap);

   /* Admin-gated allowlist upsert (org_telemetry_allow, WORM-audited). metric_names
    * is the Postgres array literal (e.g. '{a,b,c}'). Returns 0, DB2_TELEMETRY_ERR_DENIED
    * (not admin), or -1. Runs inside an open tenant scope. */
   int db2_telemetry_allow(const char *event_schema, const char *metric_names_array, int enabled);

   /* Admin-gated allowlist read (org_telemetry_allow_show). Fills out[0..n) and
    * returns n (0..max), DB2_TELEMETRY_ERR_DENIED, or -1. Runs inside an open scope. */
   int db2_telemetry_allow_show(db2_telemetry_allow_row_t *out, int max);

   /* Admin-gated authoritative-state metrics snapshot (org_metrics_snapshot). Fills
    * out[0..n) and returns n (0..max), DB2_TELEMETRY_ERR_DENIED, or -1. Runs inside
    * an open tenant scope (admin OR the owner scope opened for the scrape token). */
   int db2_metrics_snapshot(org_metric_row_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ORG_TELEMETRY_H */
