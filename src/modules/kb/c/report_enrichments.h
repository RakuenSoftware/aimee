/* kb_store/report_enrichments.h: subject-keyed report enrichment cache. */
#ifndef DEC_KB_STORE_REPORT_ENRICHMENTS_H
#define DEC_KB_STORE_REPORT_ENRICHMENTS_H 1

#include "../headers/report_enrichment.h"

#ifdef __cplusplus
extern "C"
{
#endif

   typedef struct kb_store_report_enrichment_row
   {
      report_subject_t subject;
      char enrichment_kind[64];
      char source[128];
      char schema_version[64];
      char payload_json[4096];
      char input_hash[128];
      char computed_at[32];
      char expires_at[32];
   } kb_store_report_enrichment_row_t;

   int kb_store_report_enrichment_upsert(const report_subject_t *subject,
                                         const char *enrichment_kind, const char *source,
                                         const char *schema_version, const char *payload_json,
                                         const char *input_hash, const char *computed_at,
                                         const char *expires_at);

   int kb_store_report_enrichment_read(const report_subject_t *subject, const char *enrichment_kind,
                                       const char *source, const char *schema_version,
                                       kb_store_report_enrichment_row_t *out);

   int kb_store_report_enrichment_is_expired(const kb_store_report_enrichment_row_t *row,
                                             const char *now_utc_text);

#ifdef __cplusplus
}
#endif

#endif /* DEC_KB_STORE_REPORT_ENRICHMENTS_H */
