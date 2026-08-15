/* db2/features.h: feature_rows for ranking and detection.
 * See
 * docs/proposals/accepted/statistical-decision-systems-for-ranking-calibration-and-experiments.md
 */
#ifndef DEC_DB2_FEATURES_H
#define DEC_DB2_FEATURES_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Upsert a feature row for (subject_id, subject_kind, feature_set_version).
    * features_json: flat JSON object with family-prefixed field names
    *   (lex.*, dense.*, temp.*, prov.*, etc.).
    * computed_at: UTC timestamp string; pass NULL to use current time.
    * Returns 0 on success, -1 on error. */
   int db2_feature_row_upsert(const char *subject_id, const char *subject_kind,
                              const char *scope_kind, const char *scope_id,
                              const char *feature_set_version, const char *features_json,
                              const char *computed_at);

   /* Read the features JSON for (subject_id, subject_kind, feature_set_version).
    * Returns 0 on success, -1 if not found or error. */
   int db2_feature_row_read(const char *subject_id, const char *subject_kind,
                            const char *feature_set_version, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB2_FEATURES_H */
