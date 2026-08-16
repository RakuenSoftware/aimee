/* db2/calibration.h: Bayesian promotion-threshold calibration profiles.
 *
 * Calibration profiles are stored as artifacts (kind=calibration_profile,
 * state=committed) in the DB2 artifacts table. The profile payload is a JSON
 * object with Beta-binomial bucket posteriors and a conformal floor.
 *
 * Lookup uses narrowest-scope fallback: exact (surface, kind, scope) first,
 * then (surface, kind, scope_kind with empty scope_id), then (surface, kind, "").
 *
 * See docs/proposals/done/bayesian-promotion-threshold-calibration.md */
#ifndef DEC_DB2_CALIBRATION_H
#define DEC_DB2_CALIBRATION_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define DB2_CALIBRATION_BUCKETS       10
#define DB2_CALIBRATION_CONFORMAL_MAX 512
#define DB2_CALIBRATION_SURFACE_LEN   64
#define DB2_CALIBRATION_SCOPE_LEN     128

   typedef struct
   {
      double range_lo; /* inclusive lower bound of this bucket */
      double range_hi; /* exclusive upper bound */
      double alpha;    /* Beta posterior alpha */
      double beta;     /* Beta posterior beta */
      int sample_n;    /* total audit rows in this bucket */
   } db2_calibration_bucket_t;

   typedef struct
   {
      /* Conformal abstention floor — reject if raw confidence < reject_below */
      int window_size;
      double epsilon;
      double reject_below;
      int calibration_n; /* rows used to fit the conformal quantile */
   } db2_calibration_conformal_t;

   typedef struct
   {
      double applied_confidence;
      char verdict[32];
   } db2_calibration_conformal_row_t;

   typedef struct
   {
      char target_surface[DB2_CALIBRATION_SURFACE_LEN];
      char kind[DB2_CALIBRATION_SURFACE_LEN];
      char scope_kind[DB2_CALIBRATION_SURFACE_LEN];
      char scope_id[DB2_CALIBRATION_SCOPE_LEN];
   } db2_calibration_surface_t;

   /* Parse a calibration_profile JSON payload into the promotion threshold
    * implied by its Beta buckets and conformal reject floor. static_threshold
    * is the posterior lower-bound target used when choosing the earliest
    * confidence bucket. Returns 0 on success, -1 for invalid/missing JSON. */
   int db2_calibration_threshold_from_profile_json(const char *payload_json,
                                                   double static_threshold, double *threshold_out);

   /* Write a calibration_profile artifact for (target_surface, kind,
    * scope_kind, scope_id).  payload_json is the full JSON profile blob.
    * Returns the new artifact id in id_out (>= 37 bytes) on success.
    * Returns 0 on success, -1 on error. */
   int db2_calibration_profile_write(const char *target_surface, const char *kind,
                                     const char *scope_kind, const char *scope_id,
                                     const char *feature_set_version, const char *payload_json,
                                     char *id_out, int id_out_len);

   /* Read the most recently committed calibration_profile artifact for
    * (target_surface, kind, scope_kind, scope_id) with narrowest-scope
    * fallback.  Writes the payload JSON into buf (up to len bytes).
    * Returns 0 on success, -1 if not found or error. */
   int db2_calibration_profile_read(const char *target_surface, const char *kind,
                                    const char *scope_kind, const char *scope_id, char *buf,
                                    size_t len);

   /* Aggregate audit_events into per-confidence-bucket accept/reject counts
    * for (target_surface, kind, scope_kind, scope_id) within the last
    * window_rows recent rows.  Fills up to DB2_CALIBRATION_BUCKETS buckets.
    * Returns the number of buckets filled (0..DB2_CALIBRATION_BUCKETS),
    * or -1 on error. */
   int db2_calibration_audit_stats(const char *target_surface, const char *kind,
                                   const char *scope_kind, const char *scope_id, int window_rows,
                                   db2_calibration_bucket_t *buckets, int max_buckets);

   /* Read recent audit rows for the conformal abstention window.
    * Returns rows written, or -1 on error. */
   int db2_calibration_conformal_window(const char *target_surface, const char *kind,
                                        const char *scope_kind, const char *scope_id,
                                        int window_rows, db2_calibration_conformal_row_t *rows,
                                        int max_rows);

   /* Count distinct (target_surface, kind, scope_kind, scope_id) triples that
    * have at least min_rows audit_events with a non-empty verdict.
    * Useful to decide whether to run calibration at all.
    * Returns count (>= 0) or -1 on error. */
   int db2_calibration_surfaces_with_data(int min_rows);

   /* List distinct (target_surface, kind, scope_kind, scope_id) tuples with
    * at least min_rows audit_events that have a non-empty verdict.
    * Returns rows written (>= 0) or -1 on error. */
   int db2_calibration_surface_list(int min_rows, db2_calibration_surface_t *out, int max_out);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_CALIBRATION_H */
