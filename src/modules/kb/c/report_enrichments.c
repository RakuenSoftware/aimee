/* db2/report_enrichments.c: subject-keyed report enrichment cache. */
#include "report_enrichments.h"

#include "db2_internal.h"
#include "db_postgres.h"
#include "../headers/aimee.h"

#include <stdio.h>
#include <string.h>

static int valid_subject(const report_subject_t *subject)
{
   return subject && subject->type[0] && subject->id[0] &&
          strcmp(subject->type, REPORT_SUBJECT_TYPE_AGGREGATE) != 0;
}

int db2_report_enrichment_upsert(const report_subject_t *subject, const char *enrichment_kind,
                                 const char *source, const char *schema_version,
                                 const char *payload_json, const char *input_hash,
                                 const char *computed_at, const char *expires_at)
{
   if (!valid_subject(subject) || !enrichment_kind || !enrichment_kind[0] || !schema_version ||
       !schema_version[0] || !payload_json)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   char ts[32];
   if (!computed_at || !computed_at[0])
      now_utc(ts, sizeof(ts));
   else
      snprintf(ts, sizeof(ts), "%s", computed_at);

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO report_enrichments"
       " (subject_type, subject_id, enrichment_kind, source, schema_version,"
       "  payload_json, input_hash, computed_at, expires_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"
       " ON CONFLICT (subject_type, subject_id, enrichment_kind, source, schema_version)"
       " DO UPDATE SET payload_json = EXCLUDED.payload_json,"
       "               input_hash = EXCLUDED.input_hash,"
       "               computed_at = EXCLUDED.computed_at,"
       "               expires_at = EXCLUDED.expires_at",
       err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", subject->type);
   aimee_pg_bind_text(st, "?2", subject->id);
   aimee_pg_bind_text(st, "?3", enrichment_kind);
   aimee_pg_bind_text(st, "?4", source ? source : "");
   aimee_pg_bind_text(st, "?5", schema_version);
   aimee_pg_bind_text(st, "?6", payload_json);
   aimee_pg_bind_text(st, "?7", input_hash ? input_hash : "");
   aimee_pg_bind_text(st, "?8", ts);
   aimee_pg_bind_text(st, "?9", expires_at ? expires_at : "");

   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_report_enrichment_read(const report_subject_t *subject, const char *enrichment_kind,
                               const char *source, const char *schema_version,
                               db2_report_enrichment_row_t *out)
{
   if (!valid_subject(subject) || !enrichment_kind || !enrichment_kind[0] || !schema_version ||
       !schema_version[0] || !out)
      return -1;

   void *conn = db2_conn();
   if (!conn)
      return -1;

   memset(out, 0, sizeof(*out));

   char err[256] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT payload_json, input_hash, computed_at, expires_at"
                        " FROM report_enrichments"
                        " WHERE subject_type = ?1 AND subject_id = ?2"
                        "   AND enrichment_kind = ?3 AND source = ?4 AND schema_version = ?5",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", subject->type);
   aimee_pg_bind_text(st, "?2", subject->id);
   aimee_pg_bind_text(st, "?3", enrichment_kind);
   aimee_pg_bind_text(st, "?4", source ? source : "");
   aimee_pg_bind_text(st, "?5", schema_version);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->subject = *subject;
      snprintf(out->enrichment_kind, sizeof(out->enrichment_kind), "%s", enrichment_kind);
      snprintf(out->source, sizeof(out->source), "%s", source ? source : "");
      snprintf(out->schema_version, sizeof(out->schema_version), "%s", schema_version);
      const char *payload = aimee_pg_column_text(st, 0);
      const char *hash = aimee_pg_column_text(st, 1);
      const char *computed = aimee_pg_column_text(st, 2);
      const char *expires = aimee_pg_column_text(st, 3);
      snprintf(out->payload_json, sizeof(out->payload_json), "%s", payload ? payload : "{}");
      snprintf(out->input_hash, sizeof(out->input_hash), "%s", hash ? hash : "");
      snprintf(out->computed_at, sizeof(out->computed_at), "%s", computed ? computed : "");
      snprintf(out->expires_at, sizeof(out->expires_at), "%s", expires ? expires : "");
      found = 1;
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}

int db2_report_enrichment_is_expired(const db2_report_enrichment_row_t *row,
                                     const char *now_utc_text)
{
   if (!row || !row->expires_at[0] || !now_utc_text || !now_utc_text[0])
      return 0;
   return strcmp(row->expires_at, now_utc_text) <= 0;
}
