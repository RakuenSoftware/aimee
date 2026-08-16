#include "workflow_patterns.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define WFP_ERRBUF 256

int db2_workflow_pattern_insert(const char *pattern, const char *description, const char *source,
                                const char *source_ref, double confidence, workflow_pattern_t *out)
{
   void *conn = db2_conn();
   if (!conn || !pattern)
      return -1;

   char err[WFP_ERRBUF] = "";
   void *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO workflow_patterns (pattern, description, source, source_ref, "
                        "confidence) VALUES (?1, ?2, ?3, ?4, ?5) RETURNING id",
                        err, sizeof(err));
   if (!st)
      return -1;

   aimee_pg_bind_text(st, "?1", pattern);
   aimee_pg_bind_text(st, "?2", description ? description : "");
   aimee_pg_bind_text(st, "?3", source ? source : "");
   aimee_pg_bind_text(st, "?4", source_ref ? source_ref : "");
   aimee_pg_bind_double(st, "?5", confidence);

   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (new_id < 0)
      return -1;

   if (out)
   {
      memset(out, 0, sizeof(*out));
      out->id = new_id;
      snprintf(out->pattern, sizeof(out->pattern), "%s", pattern);
      snprintf(out->description, sizeof(out->description), "%s", description ? description : "");
      snprintf(out->source, sizeof(out->source), "%s", source ? source : "");
      snprintf(out->source_ref, sizeof(out->source_ref), "%s", source_ref ? source_ref : "");
      out->confidence = confidence;
   }
   return 0;
}