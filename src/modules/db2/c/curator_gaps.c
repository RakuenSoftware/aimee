/* db2/curator_gaps.c: corpus gap detection.
 *
 * Stage 12: detect undefined_entity and dangling_reference gaps in the corpus,
 * promote them to curiosity_items via db2_curiosity_promote_corpus_gap().
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "curator_gaps.h"
#include "artifacts.h"
#include "curiosity.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CG_ERRBUF 256
#define MAX_GAPS  20

static int gap_exists(const char *subject, const char *gap_kind)
{
   void *conn = db2_conn();
   if (!conn || !subject || !gap_kind)
      return 0;
   char err[CG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*) FROM artifacts"
                                          " WHERE kind = 'gap'"
                                          "   AND state <> 'retired'"
                                          "   AND payload::jsonb->>'subject' = ?1"
                                          "   AND payload::jsonb->>'gap_kind' = ?2",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", subject);
   aimee_pg_bind_text(st, "?2", gap_kind);
   int exists = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   return exists;
}

static int write_gap_artifact(const char *subject, const char *gap_kind, const char *evidence_ref)
{
   if (gap_exists(subject, gap_kind))
      return 0;

   char artifact_id[37];
   db2_artifact_gen_id(artifact_id, sizeof(artifact_id));

   char payload[512];
   snprintf(payload, sizeof(payload),
            "{\"subject\":\"%s\",\"gap_kind\":\"%s\","
            "\"evidence_refs\":[\"%s\"]}",
            subject, gap_kind, evidence_ref);

   if (db2_artifact_write(artifact_id, "gap", "proposed", "global", "global", "corpus.gaps", 0.6,
                          payload) != 0)
      return -1;

   db2_curiosity_promote_corpus_gap(artifact_id, gap_kind, subject, evidence_ref);
   return 1;
}

static int detect_undefined_entity_gaps(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CG_ERRBUF] = "";

   /* Entities that have no citation from any doc_section source are "undefined". */
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT e.id FROM artifacts e"
                                          " WHERE e.kind = 'entity' AND e.state = 'committed'"
                                          "   AND NOT EXISTS ("
                                          "       SELECT 1 FROM artifact_citations ac"
                                          "       WHERE ac.artifact_id = e.id"
                                          "         AND ac.source_kind = 'doc_section')"
                                          " LIMIT 20",
                                          err, sizeof(err));
   if (!st)
      return -1;

   int count = 0;
   while (count < MAX_GAPS)
   {
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
         break;
      const char *entity_id = aimee_pg_column_text(st, 0);
      if (!entity_id || !entity_id[0])
         continue;
      int rc = write_gap_artifact(entity_id, "undefined_entity", entity_id);
      if (rc > 0)
         count++;
      else if (rc < 0)
         break;
   }
   aimee_pg_finalize(st);
   return count;
}

static int detect_dangling_ref_gaps(int64_t doc_id)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0)
      return -1;
   char err[CG_ERRBUF] = "";

   /* Unresolved/stale document references from this doc are dangling. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id, raw_target FROM document_references"
                        " WHERE from_doc_id = ?1 AND resolution IN ('unresolved','stale')"
                        " LIMIT 20",
                        err, sizeof(err));
   if (!st)
      return -1;

   int count = 0;
   aimee_pg_bind_int64(st, "?1", doc_id);
   while (count < MAX_GAPS)
   {
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
         break;
      int64_t ref_id = aimee_pg_column_int64(st, 0);
      const char *raw_target = aimee_pg_column_text(st, 1);
      if (!raw_target || !raw_target[0])
         continue;
      char evidence_ref[128];
      snprintf(evidence_ref, sizeof(evidence_ref), "document_reference:%lld", (long long)ref_id);
      int rc = write_gap_artifact(raw_target, "dangling_reference", evidence_ref);
      if (rc > 0)
         count++;
      else if (rc < 0)
         break;
   }
   aimee_pg_finalize(st);
   return count;
}

int db2_corpus_detect_gaps(int64_t doc_id)
{
   if (doc_id <= 0)
      return -1;

   int total = 0;
   int n = detect_undefined_entity_gaps();
   if (n < 0)
      return -1;
   total += n;

   n = detect_dangling_ref_gaps(doc_id);
   if (n < 0)
      return -1;
   total += n;

   return total;
}