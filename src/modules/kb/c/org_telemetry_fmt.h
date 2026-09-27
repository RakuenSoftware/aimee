/* db2/org_telemetry_fmt.h: pure (no-libpq) P9a telemetry helpers.
 *
 * The dependency-light half of P9a: Prometheus text rendering + label escaping,
 * the metric_name PII-structural validator, and the scrape/ingest token SHA-256 +
 * constant-time compare. Kept separate from org_telemetry.c (the db2 access layer)
 * so these are unit-testable WITHOUT the Postgres shim. Used by both the db2 layer
 * (render over a fetched snapshot) and the HTTP layer (token auth + metric_name
 * validation). Depends only on libc + OpenSSL (SHA-256). */
#ifndef DEC_DB2_ORG_TELEMETRY_FMT_H
#define DEC_DB2_ORG_TELEMETRY_FMT_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Bounds. A metric_name is capped at 128 (the DB CHECK); a model label is a
 * catalog name (<=200). value/metric are formatted numbers/identifiers. */
#define ORG_TELEMETRY_METRIC_MAX 64
#define ORG_TELEMETRY_MODEL_MAX  256
#define ORG_TELEMETRY_PERIOD_MAX 8
#define ORG_TELEMETRY_VALUE_MAX  64

   /* One row of the org_metrics_snapshot() result, pre-rendering. team<0 / period[0]
    * == 0 / model[0] == 0 mean "this metric has no such label" (that label is
    * omitted). value is the NUMERIC as decimal text (never a lossy double). */
   typedef struct
   {
      char metric[ORG_TELEMETRY_METRIC_MAX];
      long long team; /* < 0 => no team label */
      char period[ORG_TELEMETRY_PERIOD_MAX];
      char model[ORG_TELEMETRY_MODEL_MAX];
      char value[ORG_TELEMETRY_VALUE_MAX];
   } org_metric_row_t;

   /* metric_name PII-structural gate: true iff s matches ^[a-zA-Z0-9_:]{1,128}$.
    * This is the SAME rule the definer enforces — a sub / email / free content
    * cannot pass. NULL / empty / over-length / any other char => 0. */
   int org_telemetry_metric_name_valid(const char *s);

   /* Escape a Prometheus label VALUE per the text exposition format: '\\' -> '\\\\',
    * '"' -> '\\"', newline -> '\\n'. Writes a NUL-terminated result into out[cap].
    * Returns 0 on success, -1 if the escaped form (incl. NUL) does not fit (out is
    * then set to an empty string). */
   int org_telemetry_prom_escape(const char *in, char *out, size_t cap);

   /* Render a snapshot (rows[0..n), sorted by metric so each family is contiguous)
    * as Prometheus text (0.0.4): a '# HELP'/'# TYPE' pair per metric family then one
    * sample line per row. Labels are emitted only when present (bounded set: team,
    * period, model — model escaped). Unknown metric names are skipped defensively.
    * Writes a NUL-terminated body into out[cap]; returns the byte length written, or
    * -1 if the output does not fit. */
   int org_telemetry_render_prom(const org_metric_row_t *rows, int n, char *out, size_t cap);

   /* SHA-256 of s as lowercase hex into out[65] (64 hex + NUL). */
   void org_telemetry_sha256_hex(const char *s, char out[65]);

   /* Constant-time equality of two lowercase-hex strings (compares up to 64 chars +
    * the terminating length). Returns 1 iff equal, 0 otherwise. Runs in time
    * independent of WHERE the first mismatch is (no early-out), so a caller cannot
    * time-probe the expected token hash. Non-hex / wrong-length inputs return 0. */
   int org_telemetry_token_hash_eq(const char *presented_hex, const char *expected_hex);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_ORG_TELEMETRY_FMT_H */
