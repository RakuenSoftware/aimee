/* kb_document_inspector.h: bounded structural inspection for untrusted rich documents. */
#pragma once

#define KB_DOCUMENT_INSPECTOR_VERSION "structural-v1"

typedef enum
{
   KB_DOCUMENT_CLEAN = 0,
   KB_DOCUMENT_REVIEW,
   KB_DOCUMENT_REJECT,
   KB_DOCUMENT_UNSUPPORTED,
   KB_DOCUMENT_RESOURCE_LIMIT,
   KB_DOCUMENT_INVALID
} kb_document_disposition_t;

typedef struct
{
   char format[16];
   char raw_digest[65];
   char visible_text_digest[65];
   char extracted_text_digest[65];
   char first_hidden_digest[65];
   char first_hidden_channel[32];
   char lexical_verdict[32];
   int hidden_spans;
   int external_relationships;
   int active_content_flags;
   int archive_members;
   int expanded_bytes;
   int resource_limit;
   kb_document_disposition_t disposition;
} kb_document_channel_report_t;

/* Inspect bytes without executing document content, fetching relationships, or
 * loading embedded objects. The function returns 0 when it produced a report;
 * callers decide whether its disposition may proceed. Invalid arguments return
 * -1 and are also classified fail-closed in report when one is available. */
int kb_document_inspect(const char *filename, const char *bytes, int nbytes,
                        kb_document_channel_report_t *report);

const char *kb_document_disposition_name(kb_document_disposition_t disposition);
