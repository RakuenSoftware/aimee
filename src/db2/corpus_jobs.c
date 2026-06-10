/* db2/corpus_jobs.c: deterministic corpus stage conductor. */
#include "corpus_jobs.h"
#include "corpus_structural.h"
#include "curator_gaps.h"
#include "curator_terms.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define CJ_ERRBUF 256

static const char *const k_stages[] = {
    "ingested",
    "classified",
    "sectioned",
    "chunked",
    "deduplicated",
    "references_extracted",
    "terms_normalized",
    "summarized",
    "questions_generated",
    "entities_extracted",
    "claims_extracted",
    "relationships_mapped",
    "conflicts_detected",
    "gaps_detected",
    "complete",
    NULL,
};

const char *db2_corpus_pipeline_next_stage(const char *stage)
{
   const char *cur = (stage && stage[0]) ? stage : "ingested";
   for (int i = 0; k_stages[i]; i++)
      if (strcmp(k_stages[i], cur) == 0)
         return k_stages[i + 1] ? k_stages[i + 1] : NULL;
   return NULL;
}

int db2_corpus_job_seed_doc(int64_t doc_id, const char *content_hash)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0 || !content_hash || !content_hash[0])
      return -1;

   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "INSERT INTO corpus_processing_jobs"
       " (doc_id, content_hash, stage, stage_status, attempts, last_error, updated_at)"
       " VALUES (?1,?2,'ingested','pending',0,'',?3)"
       " ON CONFLICT (doc_id) DO UPDATE SET"
       " content_hash = CASE WHEN corpus_processing_jobs.content_hash <> EXCLUDED.content_hash"
       "                     THEN EXCLUDED.content_hash ELSE corpus_processing_jobs.content_hash "
       "END,"
       " stage = CASE WHEN corpus_processing_jobs.content_hash <> EXCLUDED.content_hash"
       "              THEN 'ingested' ELSE corpus_processing_jobs.stage END,"
       " stage_status = CASE WHEN corpus_processing_jobs.content_hash <> EXCLUDED.content_hash"
       "                     THEN 'pending' ELSE corpus_processing_jobs.stage_status END,"
       " attempts = CASE WHEN corpus_processing_jobs.content_hash <> EXCLUDED.content_hash"
       "                 THEN 0 ELSE corpus_processing_jobs.attempts END,"
       " last_error = CASE WHEN corpus_processing_jobs.content_hash <> EXCLUDED.content_hash"
       "                   THEN '' ELSE corpus_processing_jobs.last_error END,"
       " updated_at = EXCLUDED.updated_at",
       err, sizeof(err));
   if (!st)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_bind_text(st, "?2", content_hash);
   aimee_pg_bind_text(st, "?3", ts);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_corpus_job_record_version(int64_t doc_id, const char *scope, const char *filename,
                                  const char *content_hash)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0 || !content_hash || !content_hash[0])
      return -1;

   char doc_key[768];
   snprintf(doc_key, sizeof(doc_key), "%s:%s", scope && scope[0] ? scope : "global",
            filename && filename[0] ? filename : "");

   int64_t previous_doc_id = 0;
   int previous_version = 0;
   char previous_hash[65] = "";
   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT doc_id, version_no, content_hash FROM document_versions"
                        " WHERE doc_key = ?1 AND is_current = true"
                        " ORDER BY version_no DESC LIMIT 1",
                        err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", doc_key);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      previous_doc_id = aimee_pg_column_int64(st, 0);
      previous_version = aimee_pg_column_int(st, 1);
      db2_copy_text(previous_hash, sizeof(previous_hash), aimee_pg_column_text(st, 2));
   }
   aimee_pg_finalize(st);
   if (previous_hash[0] && strcmp(previous_hash, content_hash) == 0)
      return 0;

   char ts[32];
   now_utc(ts, sizeof(ts));
   st = aimee_pg_prepare(
       conn,
       "UPDATE document_versions SET is_current = false, superseded_at = ?1 WHERE doc_key = ?2",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_bind_text(st, "?2", doc_key);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;

   int version_no = previous_version > 0 ? previous_version + 1 : 1;
   st = aimee_pg_prepare(conn,
                         "INSERT INTO document_versions"
                         " (doc_key, doc_id, content_hash, version_no, source_time,"
                         "  source_time_provenance, is_current, superseded_at, created_at)"
                         " VALUES (?1,?2,?3,?4,'','unknown',true,'',?5)"
                         " ON CONFLICT (doc_key, content_hash) DO UPDATE SET"
                         " doc_id=EXCLUDED.doc_id, version_no=EXCLUDED.version_no,"
                         " is_current=true, superseded_at=''",
                         err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", doc_key);
   aimee_pg_bind_int64(st, "?2", doc_id);
   aimee_pg_bind_text(st, "?3", content_hash);
   aimee_pg_bind_int(st, "?4", version_no);
   aimee_pg_bind_text(st, "?5", ts);
   rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   if (rc != AIMEE_PG_DONE && rc != AIMEE_PG_ROW)
      return -1;

   if (previous_doc_id > 0 && previous_doc_id != doc_id)
   {
      st = aimee_pg_prepare(conn,
                            "UPDATE artifacts SET retired_at=?1, superseded_by=?2"
                            " WHERE id IN (SELECT artifact_id FROM artifact_citations"
                            "              WHERE source_kind='doc' AND source_id=?3)",
                            err, sizeof(err));
      if (st)
      {
         char next_id[32], prev_id[32];
         snprintf(next_id, sizeof(next_id), "%lld", (long long)doc_id);
         snprintf(prev_id, sizeof(prev_id), "%lld", (long long)previous_doc_id);
         aimee_pg_bind_text(st, "?1", ts);
         aimee_pg_bind_text(st, "?2", next_id);
         aimee_pg_bind_text(st, "?3", prev_id);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }
   return 0;
}

int db2_corpus_job_get(int64_t doc_id, db2_corpus_job_t *out)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0 || !out)
      return -1;

   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT doc_id, content_hash, stage, stage_status, attempts, last_error, updated_at"
       " FROM corpus_processing_jobs WHERE doc_id = ?1",
       err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }

   memset(out, 0, sizeof(*out));
   out->doc_id = aimee_pg_column_int64(st, 0);
   db2_copy_text(out->content_hash, sizeof(out->content_hash), aimee_pg_column_text(st, 1));
   db2_copy_text(out->stage, sizeof(out->stage), aimee_pg_column_text(st, 2));
   db2_copy_text(out->stage_status, sizeof(out->stage_status), aimee_pg_column_text(st, 3));
   out->attempts = aimee_pg_column_int(st, 4);
   db2_copy_text(out->last_error, sizeof(out->last_error), aimee_pg_column_text(st, 5));
   db2_copy_text(out->updated_at, sizeof(out->updated_at), aimee_pg_column_text(st, 6));
   aimee_pg_finalize(st);
   return 0;
}

static int corpus_event_insert(int64_t doc_id, const char *from_stage, const char *to_stage,
                               const char *outcome, const char *detail)
{
   void *conn = db2_conn();
   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO corpus_stage_events"
                        " (doc_id, from_stage, to_stage, outcome, detail, worker, created_at)"
                        " VALUES (?1,?2,?3,?4,?5,'local',?6)",
                        err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_bind_text(st, "?2", from_stage ? from_stage : "");
   aimee_pg_bind_text(st, "?3", to_stage ? to_stage : "");
   aimee_pg_bind_text(st, "?4", outcome ? outcome : "advanced");
   aimee_pg_bind_text(st, "?5", detail ? detail : "");
   aimee_pg_bind_text(st, "?6", ts);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_corpus_job_advance(int64_t doc_id, const char *outcome, const char *detail)
{
   db2_corpus_job_t job;
   if (db2_corpus_job_get(doc_id, &job) != 0)
      return -1;
   const char *next = db2_corpus_pipeline_next_stage(job.stage);
   if (!next)
      return 0;

   if (corpus_event_insert(doc_id, job.stage, next, outcome, detail) != 0)
      return -1;

   void *conn = db2_conn();
   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE corpus_processing_jobs"
                        " SET stage=?1, stage_status=?2, attempts=0, last_error='', updated_at=?3"
                        " WHERE doc_id=?4",
                        err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", next);
   aimee_pg_bind_text(st, "?2", strcmp(next, "complete") == 0 ? "complete" : "pending");
   aimee_pg_bind_text(st, "?3", ts);
   aimee_pg_bind_int64(st, "?4", doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_corpus_job_mark_restoration_candidate(int64_t doc_id, const char *content_hash,
                                              const char *signals_json)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0)
      return -1;

   db2_corpus_job_t existing;
   const char *from_stage = "";
   if (db2_corpus_job_get(doc_id, &existing) == 0)
      from_stage = existing.stage;

   char detail[512];
   snprintf(detail, sizeof(detail), "{\"restoration_candidate\":true,\"signals\":%s}",
            signals_json && signals_json[0] ? signals_json : "[]");
   if (corpus_event_insert(doc_id, from_stage, "restore", "restoration_candidate", detail) != 0)
      return -1;

   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "INSERT INTO corpus_processing_jobs"
                        " (doc_id, content_hash, stage, stage_status, attempts, last_error,"
                        "  updated_at)"
                        " VALUES (?1,?2,'restore','pending',0,'',?3)"
                        " ON CONFLICT (doc_id) DO UPDATE SET"
                        " content_hash=CASE WHEN ?2 <> '' THEN ?2 ELSE corpus_processing_jobs."
                        "content_hash END,"
                        " stage='restore', stage_status='pending', attempts=0, last_error='',"
                        " updated_at=?3",
                        err, sizeof(err));
   if (!st)
      return -1;

   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_int64(st, "?1", doc_id);
   aimee_pg_bind_text(st, "?2", content_hash ? content_hash : "");
   aimee_pg_bind_text(st, "?3", ts);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? 0 : -1;
}

int db2_corpus_job_fail(int64_t doc_id, const char *error)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0)
      return -1;
   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE corpus_processing_jobs"
       " SET stage_status='failed', attempts=attempts+1, last_error=?1, updated_at=?2"
       " WHERE doc_id=?3",
       err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", error ? error : "");
   aimee_pg_bind_text(st, "?2", ts);
   aimee_pg_bind_int64(st, "?3", doc_id);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE && changed > 0) ? 0 : -1;
}

int db2_corpus_job_recover_running(int stale_seconds)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CJ_ERRBUF] = "";
   const char *sql = "UPDATE corpus_processing_jobs SET stage_status='pending', updated_at=?1"
                     " WHERE stage_status='running'";
   (void)stale_seconds;
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   char ts[32];
   now_utc(ts, sizeof(ts));
   aimee_pg_bind_text(st, "?1", ts);
   aimee_pg_step_t rc = aimee_pg_step(st, err, sizeof(err));
   int changed = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE || rc == AIMEE_PG_ROW) ? changed : -1;
}

int db2_corpus_pipeline_status(db2_corpus_pipeline_stats_t *out)
{
   void *conn = db2_conn();
   if (!conn || !out)
      return -1;
   memset(out, 0, sizeof(*out));

   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT stage_status, COUNT(*) FROM corpus_processing_jobs"
                        " GROUP BY stage_status",
                        err, sizeof(err));
   if (!st)
      return -1;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *status = aimee_pg_column_text(st, 0);
      int count = aimee_pg_column_int(st, 1);
      out->total += count;
      if (status && strcmp(status, "pending") == 0)
         out->pending += count;
      else if (status && strcmp(status, "running") == 0)
         out->running += count;
      else if (status && strcmp(status, "failed") == 0)
         out->failed += count;
      else if (status && strcmp(status, "complete") == 0)
         out->complete += count;
   }
   aimee_pg_finalize(st);
   return 0;
}

int db2_corpus_pipeline_stage_counts(db2_corpus_pipeline_stage_count_t *out, int max_out)
{
   void *conn = db2_conn();
   if (!conn || !out || max_out <= 0)
      return -1;
   memset(out, 0, (size_t)max_out * sizeof(out[0]));

   char err[CJ_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT stage, stage_status, COUNT(*) FROM corpus_processing_jobs"
                        " GROUP BY stage, stage_status ORDER BY stage, stage_status",
                        err, sizeof(err));
   if (!st)
      return -1;

   int n = 0;
   while (n < max_out && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      db2_copy_text(out[n].stage, sizeof(out[n].stage), aimee_pg_column_text(st, 0));
      db2_copy_text(out[n].stage_status, sizeof(out[n].stage_status), aimee_pg_column_text(st, 1));
      out[n].count = aimee_pg_column_int(st, 2);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

static int corpus_run_stage_handler(int64_t doc_id, const char *next_stage, char *detail,
                                    size_t detail_len)
{
   if (strcmp(next_stage, "restore") == 0)
   {
      (void)doc_id;
      snprintf(detail, detail_len, "restoration queued");
      return 1;
   }

   if (strcmp(next_stage, "classified") == 0)
   {
      int rc = db2_corpus_classify_doc(doc_id, "pipeline");
      snprintf(detail, detail_len, "classified");
      return rc;
   }
   if (strcmp(next_stage, "sectioned") == 0)
   {
      int n = db2_corpus_sections_rebuild(doc_id);
      snprintf(detail, detail_len, "sections=%d", n < 0 ? 0 : n);
      return n < 0 ? -1 : 0;
   }
   if (strcmp(next_stage, "references_extracted") == 0)
   {
      int n = db2_corpus_extract_references(doc_id);
      snprintf(detail, detail_len, "references=%d", n < 0 ? 0 : n);
      return n < 0 ? -1 : 0;
   }
   if (strcmp(next_stage, "terms_normalized") == 0)
   {
      int n = db2_corpus_normalize_terms(doc_id);
      snprintf(detail, detail_len, "terms=%d", n < 0 ? 0 : n);
      return n < 0 ? -1 : 0;
   }
   if (strcmp(next_stage, "gaps_detected") == 0)
   {
      int n = db2_corpus_detect_gaps(doc_id);
      snprintf(detail, detail_len, "gaps=%d", n < 0 ? 0 : n);
      return n < 0 ? -1 : 0;
   }

   snprintf(detail, detail_len, "no local handler");
   return 1;
}

int db2_corpus_pipeline_drain(int limit, db2_corpus_pipeline_stats_t *out)
{
   int processed = 0;
   int max_steps = limit > 0 ? limit : 10000;

   while (processed < max_steps)
   {
      void *conn = db2_conn();
      if (!conn)
         return -1;
      char err[CJ_ERRBUF] = "";
      aimee_pg_stmt_t *st =
          aimee_pg_prepare(conn,
                           "SELECT doc_id, stage FROM corpus_processing_jobs"
                           " WHERE stage_status IN ('pending','running') AND stage <> 'complete'"
                           " ORDER BY doc_id ASC LIMIT 1",
                           err, sizeof(err));
      if (!st)
         return -1;
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
      {
         aimee_pg_finalize(st);
         break;
      }
      int64_t doc_id = aimee_pg_column_int64(st, 0);
      char stage[CORPUS_PIPELINE_STAGE_LEN];
      db2_copy_text(stage, sizeof(stage), aimee_pg_column_text(st, 1));
      aimee_pg_finalize(st);

      const char *next = db2_corpus_pipeline_next_stage(stage);
      if (!next)
         break;

      char detail[128] = "";
      int hrc = corpus_run_stage_handler(doc_id, next, detail, sizeof(detail));
      if (hrc < 0)
      {
         db2_corpus_job_fail(doc_id, detail[0] ? detail : "stage handler failed");
         return -1;
      }
      if (db2_corpus_job_advance(doc_id, hrc > 0 ? "skipped" : "advanced", detail) != 0)
         return -1;
      processed++;
   }

   if (out && db2_corpus_pipeline_status(out) != 0)
      return -1;
   if (out)
      out->processed = processed;
   return 0;
}
