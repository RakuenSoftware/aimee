/* kb_curator_extract.c: curator drain handler — claim one extract_doc job,
 * invoke the sidecar, write artifacts to DB2, mark job done/failed.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_extract.h"
#include "kb_curator_sidecar.h"
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

#include <pthread.h> /* reclaim throttle is shared across the doc workers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CE_ERRBUF                256
#define CE_DOCBUF                65536
#define CE_OUTBUF                (256 * 1024)
#define CE_EXTRACT_MAX_ARTIFACTS 5

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
 * request drive specifics. The shared response schema caps the artifact array so
 * the bundled CPU model can close valid JSON inside the curator token budget. */
#define CE_SYSTEM_PROMPT                                                                           \
   "You are a knowledge-base extractor. Read the JSON request (a document chunk "                  \
   "with a `role` and `input.content`) and emit the requested entities, claims, "                  \
   "and relations grounded only in that content. Respond with a single JSON "                      \
   "object matching the request's role. Emit at most five artifacts, prioritize "                  \
   "the most important facts, and keep every string concise. Do not invent facts."

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
   "Emit at most five artifacts total: exactly one doc_summary, then the most important claims "   \
   "and entities. Keep the summary under 60 words and every other string under 20 words."

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
   cJSON_AddNumberToObject(arts, "maxItems", CE_EXTRACT_MAX_ARTIFACTS);
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
   int64_t generation;
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
                            "     AND EXISTS (SELECT 1 FROM kb_documents d"
                            "       JOIN projects p ON p.name=d.project"
                            "       WHERE d.id=kb_async_jobs.document_id"
                            "         AND p.lifecycle_state='current'"
                            "         AND d.generation=p.current_generation)"
                            /* Skip jobs still serving their retry backoff. '' is
                             * "never failed" and must always be claimable. */
                            "     AND (next_attempt_at = '' OR next_attempt_at <= ?1)"
                            "   ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED"
                            " )"
                            " RETURNING id, document_id, project, attempts";

   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   char now_text[32];
   kb_curator_now_text(now_text, sizeof(now_text));
   aimee_pg_bind_text(st, "?1", now_text);

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
   static const char *sql = "SELECT d.file_path,d.heading_path,d.content,d.generation"
                            " FROM kb_documents d JOIN projects p ON p.name=d.project"
                            " WHERE d.id=?1 AND d.doc_kind<>'pdf'"
                            " AND p.lifecycle_state='current'"
                            " AND d.generation=p.current_generation";

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
      /* These cuts are byte-wise, so a doc chunk longer than the buffer has its
       * final multi-byte character split. The partial bytes flow into the request
       * JSON and then the artifact payload, where postgres rejects the INSERT
       * with "invalid byte sequence for encoding UTF8" — the job dies for one
       * stray em-dash at the cut point. Trim back to a character boundary. */
      snprintf(job->file_path, sizeof(job->file_path), "%s", fp ? fp : "");
      text_trim_partial_utf8(job->file_path);
      snprintf(job->heading_path, sizeof(job->heading_path), "%s", hp ? hp : "");
      text_trim_partial_utf8(job->heading_path);
      snprintf(job->content, sizeof(job->content), "%s", ct ? ct : "");
      text_trim_partial_utf8(job->content);
      job->generation = aimee_pg_column_int64(st, 3);
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

/* Requeue WITHOUT spending an attempt: undo the increment ce_claim_job applied,
 * and back off.
 *
 * A provider outage used to burn the whole budget in seconds. The upstream
 * gateway opens a circuit breaker and then refuses everything for its cooldown,
 * so three claims inside one cooldown drove a job straight to terminal 'failed'
 * — permanent, unreviewed data loss from a condition that heals itself a minute
 * later. Observed on a live deployment: every failed row sat at exactly
 * attempts=3 with last_error "provider HTTP 503".
 *
 * The budget exists to stop a POISON JOB looping forever. An outage is not a
 * poison job, and spending the budget on it protects nothing. The row stays
 * 'pending' for as long as the provider is down; next_attempt_at bounds the
 * retry load, and the operator sees the backlog in `aimee kb status`. */
void kb_curator_mark_retry_provider_unavailable(int64_t job_id, int attempts, const char *error_msg)
{
   /* Arm the global gate even if the DB pool is the resource currently starved;
    * otherwise a failed requeue lookup would let the next fresh row hammer the
    * same open circuit immediately. The still-running row is reclaimed by the
    * existing stale-lease path after restart/recovery. */
   kb_curator_provider_backoff_note();
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "UPDATE kb_async_jobs"
                        " SET status = 'pending', last_error = ?1,"
                        "     attempts = CASE WHEN attempts > 0 THEN attempts - 1 ELSE 0 END,"
                        "     next_attempt_at = ?3, updated_at = pg_now_text()"
                        " WHERE id = ?2",
                        err, sizeof(err));
   if (!st)
      return;
   char errbuf[512];
   snprintf(errbuf, sizeof(errbuf), "%s", error_msg ? error_msg : "provider unavailable");
   aimee_pg_bind_text(st, "?1", errbuf);
   aimee_pg_bind_int64(st, "?2", job_id);
   /* Back off on the attempt number we just gave back, so a long outage still
    * lengthens the interval instead of hammering a breaker that is open. */
   char next_at[32];
   kb_curator_next_attempt_at(attempts > 0 ? attempts : 1, next_at, sizeof(next_at));
   aimee_pg_bind_text(st, "?3", next_at);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static int ce_mark_retry_or_fail(int64_t job_id, int attempts, int max_attempts,
                                 const char *error_msg)
{
   if (kb_curator_error_is_provider_unavailable(error_msg))
   {
      kb_curator_mark_retry_provider_unavailable(job_id, attempts, error_msg);
      return 1;
   }
   void *conn = db2_conn();
   if (!conn)
      return 0;

   const char *new_status = (attempts >= max_attempts) ? "failed" : "pending";
   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE kb_async_jobs"
                                          " SET status = ?1, last_error = ?2, next_attempt_at = ?4,"
                                          "     updated_at = pg_now_text()"
                                          " WHERE id = ?3",
                                          err, sizeof(err));
   if (!st)
      return 0;
   /* Back the retry off. A terminal 'failed' still gets a stamp — harmless, and
    * it keeps the column meaningful if the job is ever re-queued by hand. */
   char next_at[32];
   kb_curator_next_attempt_at(attempts, next_at, sizeof(next_at));
   aimee_pg_bind_text(st, "?4", next_at);
   aimee_pg_bind_text(st, "?1", new_status);
   char errbuf[512];
   snprintf(errbuf, sizeof(errbuf), "%s", error_msg ? error_msg : "unknown error");
   aimee_pg_bind_text(st, "?2", errbuf);
   aimee_pg_bind_int64(st, "?3", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return 0;
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
 * point once per job.
 *
 * The throttle is mutex-guarded because kb_curator_extract_one runs on the main
 * LLM lane AND on every kb_curator_extract_docs_workers thread, so the state is
 * genuinely shared. The lock is held across the UPDATE, which is free in
 * practice: it is taken once per CE_RECLAIM_EVERY_S, and workers that lose the
 * race just observe the throttle and return. */
static void ce_reclaim_stale_running(int max_attempts)
{
   static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
   static time_t last_run = 0;

   if (max_attempts < 1)
      max_attempts = 1;

   pthread_mutex_lock(&lock);
   time_t now = time(NULL);
   if (last_run != 0 && now - last_run < CE_RECLAIM_EVERY_S)
   {
      pthread_mutex_unlock(&lock);
      return;
   }

   void *conn = db2_conn();
   if (!conn)
   {
      /* Deliberately do NOT arm the throttle here, nor on the failures below: a
       * reclaim that never ran must not suppress the next attempt for a minute.
       * Only a completed UPDATE counts as "ran recently". */
      pthread_mutex_unlock(&lock);
      return;
   }
   char err[CE_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE kb_async_jobs"
       " SET status = CASE WHEN attempts >= ?1 THEN 'failed' ELSE 'pending' END,"
       "     claimed_by = '', claimed_at = '',"
       /* Only fill last_error when the worker left none: an existing message is
        * the diagnostic from the attempt that died, and 'reclaimed after max
        * attempts' only says we gave up — overwriting would destroy the reason. */
       "     last_error = CASE WHEN attempts >= ?1 AND last_error = ''"
       "                       THEN 'stale running lease reclaimed after max attempts'"
       "                       ELSE last_error END,"
       "     updated_at = pg_now_text()"
       " WHERE kind = 'extract_doc' AND status = 'running'"
       "   AND claimed_at <> ''"
       /* Compare the lease by VALUE, not as raw text. pg_now_text() is canonical
        * ISO, but a row claimed before that became canonical still carries the
        * space separator, and a text compare decides at character 10 where ' '
        * (0x20) sorts below 'T' (0x54) -- so a legacy row read as older than any
        * same-date threshold and its live lease was reclaimed out from under it.
        * Lease horizons are minutes, so "same date" is the normal case here, not
        * an edge. */
       "   AND rtrim(replace(claimed_at,'T',' '),'Z')"
       "     < rtrim(replace(pg_now_text(?2),'T',' '),'Z')",
       err, sizeof(err));
   if (!st)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "stale-lease reclaim could not prepare: %s", err);
      pthread_mutex_unlock(&lock);
      return;
   }
   aimee_pg_bind_int(st, "?1", max_attempts);
   aimee_pg_bind_text(st, "?2", CE_STALE_LEASE);
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      last_run = now; /* only a completed UPDATE arms the throttle */
   else
      aimee_log(LOG_WARN, "kb.curator.extract", "stale-lease reclaim failed: %s", err);
   aimee_pg_finalize(st);
   pthread_mutex_unlock(&lock);
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
      if (cJSON_IsObject(payload_j))
      {
         cJSON *payload = (cJSON *)payload_j;
         cJSON_DeleteItemFromObject(payload, "project");
         cJSON_AddStringToObject(payload, "project", job->project);
         cJSON_DeleteItemFromObject(payload, "generation");
         cJSON_AddNumberToObject(payload, "generation", job->generation);
      }
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
   if (kb_curator_provider_backoff_active())
      return 0;

   ce_job_t job;
   memset(&job, 0, sizeof(job));

   /* Recover jobs orphaned in 'running' before claiming fresh work, so a crash/
    * restart or a previously-wedged sidecar cannot permanently strand them. */
   ce_reclaim_stale_running(opts ? opts->max_attempts : 1);

   if (!ce_claim_job(&job))
   {
      /* A cooldown may have elapsed after this queue emptied. Do not carry its
       * exponential history into an unrelated future outage. */
      kb_curator_provider_backoff_recovered();
      return 0; /* queue empty */
   }

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
    * bundled-Gemma SYNTHESIS_ENDPOINT env) runs in-process via provider_client; else
    * the resolved sidecar command. extract_docs is Tier-A. */
   aimee_log(LOG_INFO, "kb.curator.extract", "invoking curator LLM for doc %lld (cmd fallback: %s)",
             (long long)job.document_id, cmd);

   /* The model can legitimately take minutes on the bundled CPU backend. The
    * durable job is already claimed, and every remaining DB operation can
    * re-acquire lazily, so do not pin a pool member across the network call. */
   db2_lease_release_idle();

   char sidecar_err[512] = "";
   const char *sys_prompt = novel_mode ? CE_SYSTEM_PROMPT : CE_EXTRACT_DOC_PROMPT;
   cJSON *extract_schema = ce_build_extract_schema();
   char *resp_str =
       kb_curator_llm_run(KB_CURATOR_STAGE_EXTRACT_DOCS, sys_prompt, req_str, extract_schema, cmd,
                          CE_OUTBUF, sidecar_err, sizeof(sidecar_err));
   cJSON_Delete(extract_schema);
   free(req_str);

   if (!resp_str)
   {
      aimee_log(LOG_WARN, "kb.curator.extract", "sidecar failed for job %lld (attempt %d/%d): %s",
                (long long)job.job_id, job.attempts, opts->max_attempts, sidecar_err);
      int provider_down =
          ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, sidecar_err);
      if (!provider_down)
         kb_curator_provider_backoff_recovered();
      return provider_down ? -1 : 1;
   }

   kb_curator_provider_backoff_recovered();

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
      int provider_down =
          ce_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, errmsg);
      cJSON_Delete(resp);
      return provider_down ? -1 : 1;
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
