/* kb_curator_extract.c: curator drain handler — claim one extract_doc job,
 * invoke the sidecar, write artifacts to DB2, mark job done/failed.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_extract.h"
#include "kb_curator_llm.h"
#include "aimee.h"
#include "config.h" /* config_current_mode, aimee_mode_t, config_load */
#include "cJSON.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"
#include "db2/feature_rows.h"
#include "kb_mdl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CE_ERRBUF 256
#define CE_DOCBUF 65536
#define CE_OUTBUF (256 * 1024)

/* System prompt for the extract stage (Tier-A) when routed through a configured
 * provider (§2b). The legacy python sidecar carried its own prompt; the in-process
 * provider needs one here. The request JSON's `role`/`prompt_version` select the
 * extraction shape (engineering doc vs novel story); keep this generic and let the
 * request drive specifics. Grammar-constrained output is a future enhancement
 * (kb_curator_llm_run does not yet pass a JSON schema). Tune against the model. */
#define CE_SYSTEM_PROMPT                                                                           \
   "You are a knowledge-base extractor. Read the JSON request (a document chunk "                  \
   "with a `role` and `input.content`) and emit the requested entities, claims, "                  \
   "and relations grounded only in that content. Respond with a single JSON "                      \
   "object matching the request's role. Do not invent facts."

typedef struct
{
   int64_t job_id;
   int64_t document_id;
   char project[256];
   int attempts;
   char file_path[1024];
   char heading_path[512];
   char content[CE_DOCBUF];
} ce_job_t;

/* ── DB helpers ─────────────────────────────────────────────────────────── */

static int ce_claim_job(ce_job_t *out)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE kb_async_jobs"
                            " SET status = 'running',"
                            "     claimed_by = 'kb.curator.drain',"
                            "     claimed_at = pg_now_text(),"
                            "     attempts   = attempts + 1,"
                            "     updated_at = pg_now_text()"
                            " WHERE id = ("
                            "   SELECT id FROM kb_async_jobs"
                            "   WHERE kind = 'extract_doc' AND status = 'pending'"
                            "   ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED"
                            " )"
                            " RETURNING id, document_id, project, attempts";

   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->job_id = aimee_pg_column_int64(st, 0);
      out->document_id = aimee_pg_column_int64(st, 1);
      const char *proj = aimee_pg_column_text(st, 2);
      snprintf(out->project, sizeof(out->project), "%s", proj ? proj : "");
      out->attempts = aimee_pg_column_int(st, 3);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

static int ce_fetch_document(ce_job_t *job)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT file_path, heading_path, content"
                            " FROM kb_documents WHERE id = ?1";

   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", job->document_id);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *fp = aimee_pg_column_text(st, 0);
      const char *hp = aimee_pg_column_text(st, 1);
      const char *ct = aimee_pg_column_text(st, 2);
      snprintf(job->file_path, sizeof(job->file_path), "%s", fp ? fp : "");
      snprintf(job->heading_path, sizeof(job->heading_path), "%s", hp ? hp : "");
      snprintf(job->content, sizeof(job->content), "%s", ct ? ct : "");
      found = 1;
   }
   aimee_pg_finalize(st);
   return found ? 0 : -1;
}

static void ce_mark_done(int64_t job_id)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE kb_async_jobs SET status='done', updated_at=pg_now_text() WHERE id=?1", err,
       sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void ce_mark_retry_or_fail(int64_t job_id, int attempts, int max_attempts,
                                  const char *error_msg)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   const char *new_status = (attempts >= max_attempts) ? "failed" : "pending";
   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE kb_async_jobs"
                        " SET status = ?1, last_error = ?2, updated_at = pg_now_text()"
                        " WHERE id = ?3",
                        err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", new_status);
   char errbuf[512];
   snprintf(errbuf, sizeof(errbuf), "%s", error_msg ? error_msg : "unknown error");
   aimee_pg_bind_text(st, "?2", errbuf);
   aimee_pg_bind_int64(st, "?3", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

/* Resolve sidecar command: use opts->extract_command if set, else find
 * scripts/curator-extract.py relative to the running binary. */
static void ce_resolve_command(const kb_curator_extract_opts_t *opts, char *out, size_t len)
{
   if (opts->extract_command[0])
   {
      snprintf(out, len, "%s", opts->extract_command);
      return;
   }

   /* Try /proc/self/exe on Linux to locate the binary and find scripts/ */
   char exe[512];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (n > 0)
   {
      exe[n] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash)
      {
         *slash = '\0';
         /* binary is in <build>/; scripts/ is at <repo>/scripts/ */
         char candidate[768];
         snprintf(candidate, sizeof(candidate), "%s/../scripts/curator-extract.py", exe);
         if (access(candidate, X_OK) == 0)
         {
            snprintf(out, len, "python3 %s", candidate);
            return;
         }
      }
   }

   snprintf(out, len, "python3 scripts/curator-extract.py");
}

/* ── Artifact write ─────────────────────────────────────────────────────── */

static int ce_write_artifacts(const ce_job_t *job, cJSON *artifacts_arr)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CE_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   char doc_id_str[32];
   snprintf(doc_id_str, sizeof(doc_id_str), "%lld", (long long)job->document_id);

   int n = cJSON_GetArraySize(artifacts_arr);
   for (int i = 0; i < n; i++)
   {
      cJSON *item = cJSON_GetArrayItem(artifacts_arr, i);
      if (!item)
         continue;

      const cJSON *kind_j = cJSON_GetObjectItemCaseSensitive(item, "kind");
      const cJSON *payload_j = cJSON_GetObjectItemCaseSensitive(item, "payload");
      const cJSON *conf_j = cJSON_GetObjectItemCaseSensitive(item, "confidence");

      if (!cJSON_IsString(kind_j) || !kind_j->valuestring[0])
         continue;

      double confidence = cJSON_IsNumber(conf_j) ? conf_j->valuedouble : 0.80;
      char *payload_str = payload_j ? cJSON_PrintUnformatted(payload_j) : NULL;

      char id_buf[64];
      db2_artifact_gen_id(id_buf, sizeof(id_buf));

      int wrc =
          db2_artifact_write(id_buf, kind_j->valuestring, "proposed", "project", job->project,
                             "kb.curator.extract", confidence, payload_str ? payload_str : "{}");

      if (wrc != 0)
      {
         free(payload_str);
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }

      db2_artifact_cite(id_buf, "kb_document", doc_id_str);

      /* Emit mdl.* features for this synthesis candidate. Evidence = document
       * content.  Prefer payload["text"] then payload["body"] as synthesis text
       * so the score reflects the human-readable claim, not the full JSON. */
      {
         const char *synth = NULL;
         const cJSON *txt_j =
             payload_j ? cJSON_GetObjectItemCaseSensitive(payload_j, "text") : NULL;
         const cJSON *bdy_j =
             payload_j ? cJSON_GetObjectItemCaseSensitive(payload_j, "body") : NULL;
         if (cJSON_IsString(txt_j) && txt_j->valuestring[0])
            synth = txt_j->valuestring;
         else if (cJSON_IsString(bdy_j) && bdy_j->valuestring[0])
            synth = bdy_j->valuestring;
         else if (payload_str)
            synth = payload_str;

         if (synth && synth[0] && job->content[0])
         {
            kb_mdl_score_t mdl = {0};
            if (kb_mdl_score(synth, job->content, &mdl) == 0)
            {
               char feat[256];
               snprintf(feat, sizeof(feat),
                        "{\"mdl.l_candidate\":%.2f,\"mdl.l_residual\":%.2f,"
                        "\"mdl.total\":%.2f,\"mdl.rank_in_cluster\":%d}",
                        mdl.l_candidate, mdl.l_residual, mdl.total, mdl.rank_in_cluster);
               db2_feature_row_upsert(id_buf, "kb_artifact", "", "", "v1", feat, NULL);
            }
         }
      }

      free(payload_str);
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;

   return n;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

int kb_curator_extract_one(const kb_curator_extract_opts_t *opts)
{
   ce_job_t job;
   memset(&job, 0, sizeof(job));

   if (!ce_claim_job(&job))
      return 0; /* queue empty */

   aimee_log(LOG_DEBUG, "kb.curator.extract",
             "claimed extract_doc job %lld for doc %lld project '%s'", (long long)job.job_id,
             (long long)job.document_id, job.project);

   if (ce_fetch_document(&job) != 0)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "doc %lld not found for job %lld; marking failed",
                (long long)job.document_id, (long long)job.job_id);
      ce_mark_retry_or_fail(job.job_id, opts->max_attempts, opts->max_attempts,
                            "kb_documents row not found");
      return 1;
   }

   /* Build stdin JSON. In novel mode the same ingest queue feeds the
    * story-world extraction prompt (characters/locations/edges/canon facts)
    * instead of the engineering doc prompt; the sidecar, artifact pipeline and
    * queue are unchanged — only the extraction prompt differs. Engineer mode is
    * byte-identical to before. */
   int novel_mode = (config_current_mode() == AIMEE_MODE_NOVEL);
   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", novel_mode ? "extract_story" : "extract_doc");
   cJSON_AddStringToObject(req, "model_version", "");
   cJSON_AddStringToObject(req, "prompt_version",
                           novel_mode ? "curator-extract-story-v1" : "curator-extract-v1");
   cJSON *scope = cJSON_AddObjectToObject(req, "scope");
   cJSON_AddStringToObject(scope, "kind", "project");
   cJSON_AddStringToObject(scope, "id", job.project);
   cJSON *inp = cJSON_AddObjectToObject(req, "input");
   char doc_id_str[32];
   snprintf(doc_id_str, sizeof(doc_id_str), "%lld", (long long)job.document_id);
   cJSON_AddStringToObject(inp, "document_id", doc_id_str);
   cJSON_AddStringToObject(inp, "project", job.project);
   cJSON_AddStringToObject(inp, "file_path", job.file_path);
   cJSON_AddStringToObject(inp, "heading_path", job.heading_path);
   cJSON_AddStringToObject(inp, "content", job.content);
   cJSON *conf = cJSON_AddObjectToObject(req, "config");
   cJSON_AddNumberToObject(conf, "max_tokens", opts->max_tokens > 0 ? opts->max_tokens : 2048);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
   {
      ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts,
                            "failed to serialize request JSON");
      return 1;
   }

   char cmd[768];
   ce_resolve_command(opts, cmd, sizeof(cmd));

   /* Route through the §2 dispatch: a configured Tier-A provider (incl. the
    * bundled-Gemma LLM_ENDPOINT env) runs in-process via provider_client; else
    * the resolved sidecar command. extract_docs is Tier-A. */
   config_t cfg;
   config_load(&cfg);

   aimee_log(LOG_INFO, "kb.curator.extract", "invoking curator LLM for doc %lld (cmd fallback: %s)",
             (long long)job.document_id, cmd);

   char sidecar_err[512] = "";
   char *resp_str = kb_curator_llm_run(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, CE_SYSTEM_PROMPT,
                                       req_str, cmd, CE_OUTBUF, sidecar_err, sizeof(sidecar_err));
   free(req_str);

   if (!resp_str)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "sidecar failed for job %lld (attempt %d/%d): %s",
                (long long)job.job_id, job.attempts, opts->max_attempts, sidecar_err);
      ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, sidecar_err);
      return 1;
   }

   cJSON *resp = cJSON_Parse(resp_str);
   free(resp_str);

   if (!resp)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "sidecar returned non-JSON for job %lld",
                (long long)job.job_id);
      ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts,
                            "sidecar returned non-JSON");
      return 1;
   }

   const cJSON *status_j = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status_j) || strcmp(status_j->valuestring, "ok") != 0)
   {
      const cJSON *err_j = cJSON_GetObjectItemCaseSensitive(resp, "error");
      const char *errmsg = cJSON_IsString(err_j) ? err_j->valuestring : "sidecar status != ok";
      aimee_log(LOG_WARN, "kb.curator.extract", "sidecar error for job %lld: %s",
                (long long)job.job_id, errmsg);
      ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, errmsg);
      cJSON_Delete(resp);
      return 1;
   }

   cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   int n_written = 0;
   if (cJSON_IsArray(artifacts))
      n_written = ce_write_artifacts(&job, artifacts);

   cJSON_Delete(resp);

   if (n_written < 0)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "artifact write failed for job %lld",
                (long long)job.job_id);
      ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, "artifact write failed");
      return 1;
   }

   aimee_log(LOG_INFO, "kb.curator.extract", "wrote %d artifact(s) for doc %lld (job %lld)",
             n_written, (long long)job.document_id, (long long)job.job_id);
   ce_mark_done(job.job_id);
   return 1;
}
