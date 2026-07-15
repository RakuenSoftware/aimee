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

/* A job left in 'running' longer than this lease was orphaned (worker crash/
 * restart or a wedged sidecar). Mirrors the code-unit stage's lease; both sit
 * comfortably above that stage's sidecar wall-clock cap. */
#define CE_STALE_LEASE "-15 minutes"
/* Reclaim runs at most this often (throttled; the drain calls the entry per job). */
#define CE_RECLAIM_EVERY_S 60

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

/* Engineer-mode extract_doc contract: names the exact artifact kinds + payload
 * fields (claim => subject/attribute/value, matching index_claims) so the provider
 * path yields claims the contradiction detector can index. Paired with the envelope
 * schema (ce_build_extract_schema) that forces fence-free {status,artifacts} JSON —
 * without both, the reasoning model markdown-fences its output and invents an
 * envelope, so cJSON_Parse fails ("sidecar returned non-JSON"). */
#define CE_EXTRACT_DOC_PROMPT                                                                      \
   "You are a knowledge-base extractor. Read the JSON request (a document chunk with a `role` "    \
   "and `input.content`) and extract structured knowledge grounded ONLY in that content; do not "  \
   "invent facts. Respond with a single JSON object: {\"status\":\"ok\",\"artifacts\":[...]}. "    \
   "Each "                                                                                         \
   "artifact is {\"kind\":<kind>,\"payload\":<obj>}. Use these kinds and exact payload fields:\n"  \
   "- \"doc_summary\" (exactly one): {\"summary\":<one "                                           \
   "paragraph>,\"status\":\"draft|accepted|done|"                                                  \
   "rejected|deferred\",\"priority\":\"low|medium|high\",\"components\":[<lowercase tags>]}\n"     \
   "- \"claim\": {\"subject\":<what the claim is about>,\"attribute\":<the property/relation "     \
   "asserted>,\"value\":<the asserted "                                                            \
   "value>,\"claim_kind\":\"fact|requirement|constraint|decision|"                                 \
   "behavior\",\"text\":<full claim as one sentence>}\n"                                           \
   "- \"entity\": {\"name\":<canonical name>,\"entity_kind\":\"component|system|concept|protocol|" \
   "data_store|tool\",\"context\":<short phrase>}\n"                                               \
   "Emit a claim for each distinct factual assertion and an entity for each named "                \
   "system/component/"                                                                             \
   "concept."

/* Build the extract envelope json-schema (caller frees). provider_client wraps it
 * as response_format:{json_schema:{schema:<this>,strict}}. */
static cJSON *ce_build_extract_schema(void)
{
   const char *ok_only[] = {"ok"};
   const char *art_req[] = {"kind", "payload"};
   const char *top_req[] = {"status", "artifacts"};
   cJSON *sc = cJSON_CreateObject();
   cJSON_AddStringToObject(sc, "type", "object");
   cJSON *props = cJSON_AddObjectToObject(sc, "properties");
   cJSON *status = cJSON_AddObjectToObject(props, "status");
   cJSON_AddStringToObject(status, "type", "string");
   cJSON_AddItemToObject(status, "enum", cJSON_CreateStringArray(ok_only, 1));
   cJSON *arts = cJSON_AddObjectToObject(props, "artifacts");
   cJSON_AddStringToObject(arts, "type", "array");
   cJSON *items = cJSON_AddObjectToObject(arts, "items");
   cJSON_AddStringToObject(items, "type", "object");
   cJSON *iprops = cJSON_AddObjectToObject(items, "properties");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(iprops, "kind"), "type", "string");
   cJSON_AddStringToObject(cJSON_AddObjectToObject(iprops, "payload"), "type", "object");
   cJSON_AddItemToObject(items, "required", cJSON_CreateStringArray(art_req, 2));
   cJSON_AddItemToObject(sc, "required", cJSON_CreateStringArray(top_req, 2));
   return sc;
}

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

   /* Defense in depth: skip structured-PDF chunks even if one ever reaches the curator
    * queue (the enqueue in kb_curator_queue.c already excludes doc_kind='pdf'). PDF content
    * must never become a searchable derived artifact outside the access-gated search_chunks
    * path — `doc_kind <> 'pdf'` here guarantees the extractor never reads it. */
   static const char *sql = "SELECT file_path, heading_path, content"
                            " FROM kb_documents WHERE id = ?1 AND doc_kind <> 'pdf'";

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

/* Reclaim extract_doc jobs orphaned in 'running'. ce_claim_job only ever selects
 * status='pending', so a job whose worker crashed/restarted or whose sidecar
 * wedged stays 'running' forever: the document is never extracted, and the row
 * pins a db2 pool member well past its 300s ceiling ("missed lease_end?"). The
 * code-unit stage has had this guard since it shipped; kb_async_jobs never got
 * one, which stranded a job for 15h in production. Reset rows older than the
 * lease to 'pending' so they retry, or to 'failed' once attempts are exhausted
 * so a poison job cannot loop. attempts is preserved (incremented at claim).
 *
 * Scoped to kind='extract_doc': kb_async_jobs is shared with other kinds that
 * own their own claim/lease lifecycle, and reclaiming those from here would
 * yank jobs out from under their stage. Throttled — the drain calls the entry
 * point once per job. */
static void ce_reclaim_stale_running(int max_attempts)
{
   static time_t last_run = 0;
   time_t now = time(NULL);
   if (last_run != 0 && now - last_run < CE_RECLAIM_EVERY_S)
      return;
   last_run = now;

   if (max_attempts < 1)
      max_attempts = 1;

   void *conn = db2_conn();
   if (!conn)
      return;
   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE kb_async_jobs"
       " SET status = CASE WHEN attempts >= ?1 THEN 'failed' ELSE 'pending' END,"
       "     claimed_by = '', claimed_at = '',"
       "     last_error = CASE WHEN attempts >= ?1"
       "                       THEN 'stale running lease reclaimed after max attempts'"
       "                       ELSE last_error END,"
       "     updated_at = pg_now_text()"
       " WHERE kind = 'extract_doc' AND status = 'running'"
       "   AND claimed_at <> '' AND claimed_at < pg_now_text(?2)",
       err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int(st, "?1", max_attempts);
   aimee_pg_bind_text(st, "?2", CE_STALE_LEASE);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

/* Pick the curator-extract sidecar command from a candidate list. Shared by the
 * doc and code extract stages and exposed for testing. The script is invoked as
 * `python3 <path>`, so it only needs to be READABLE — the previous code checked
 * access(X_OK), which rejected the shipped 0644 script. */
void kb_curator_pick_sidecar_command(const char *explicit_cmd, const char *const *candidates,
                                     int n_candidates, char *out, size_t len)
{
   if (explicit_cmd && explicit_cmd[0])
   {
      snprintf(out, len, "%s", explicit_cmd);
      return;
   }
   for (int i = 0; i < n_candidates; i++)
   {
      if (candidates[i] && candidates[i][0] && access(candidates[i], R_OK) == 0)
      {
         snprintf(out, len, "python3 %s", candidates[i]);
         return;
      }
   }
   /* Last resort: cwd-relative — only resolves when run from a repo checkout. */
   snprintf(out, len, "python3 scripts/curator-extract.py");
}

/* Resolve the sidecar command, searching the real install locations for
 * curator-extract.py. The container images COPY scripts to /opt/aimee/scripts
 * (binary at /usr/local/bin), so the old <bindir>/../scripts heuristic computed
 * /usr/local/scripts and never matched — falling back to a cwd-relative path
 * that does not resolve from the kb working dir (/var/lib/aimee), stalling every
 * extract job. Try the image path first, then the dev/build tree. */
void kb_curator_resolve_sidecar_command(const kb_curator_extract_opts_t *opts, char *out,
                                        size_t len)
{
   char exe_rel[768] = "";
   char exe[512];
   ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
   if (n > 0)
   {
      exe[n] = '\0';
      char *slash = strrchr(exe, '/');
      if (slash)
      {
         *slash = '\0';
         snprintf(exe_rel, sizeof(exe_rel), "%s/../scripts/curator-extract.py", exe);
      }
   }
   const char *candidates[] = {
       "/opt/aimee/scripts/curator-extract.py", /* container image (Dockerfile COPY target) */
       exe_rel[0] ? exe_rel : NULL,             /* dev/build tree: binary beside scripts/ */
   };
   kb_curator_pick_sidecar_command(opts->extract_command, candidates,
                                   (int)(sizeof(candidates) / sizeof(candidates[0])), out, len);
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

   /* Recover jobs orphaned in 'running' before claiming fresh work, so a crash/
    * restart or a previously-wedged sidecar cannot permanently strand them. */
   ce_reclaim_stale_running(opts ? opts->max_attempts : 1);

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
   kb_curator_resolve_sidecar_command(opts, cmd, sizeof(cmd));

   /* Route through the §2 dispatch: a configured Tier-A provider (incl. the
    * bundled-Gemma LLM_ENDPOINT env) runs in-process via provider_client; else
    * the resolved sidecar command. extract_docs is Tier-A. */
   config_t cfg;
   config_load(&cfg);

   aimee_log(LOG_INFO, "kb.curator.extract", "invoking curator LLM for doc %lld (cmd fallback: %s)",
             (long long)job.document_id, cmd);

   char sidecar_err[512] = "";
   const char *sys_prompt = novel_mode ? CE_SYSTEM_PROMPT : CE_EXTRACT_DOC_PROMPT;
   cJSON *extract_schema = ce_build_extract_schema();
   char *resp_str =
       kb_curator_llm_run(&cfg, KB_CURATOR_STAGE_EXTRACT_DOCS, sys_prompt, req_str, extract_schema,
                          cmd, CE_OUTBUF, sidecar_err, sizeof(sidecar_err));
   cJSON_Delete(extract_schema);
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
