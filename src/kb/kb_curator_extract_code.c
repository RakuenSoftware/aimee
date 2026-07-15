/* kb_curator_extract_code.c: curator drain handler — claim one extract_code_unit job,
 * read source body from filesystem, invoke sidecar, write code_unit artifacts to DB2,
 * mark job done/failed.
 * No DB1 access from this file. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "kb_curator_extract.h"
#include "kb_curator_grounding.h"
#include "kb_curator_sidecar.h" /* kb_curator_describe_wait_status */
#include "aimee.h"
#include "cJSON.h"
#include "log.h"
#include "db2/artifacts.h"
#include "db2/db2_internal.h"
#include "db2/db_postgres.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CCU_ERRBUF     256
#define CCU_OUTBUF     (256 * 1024)
#define CCU_BODY_LINES 100

/* Hard wall-clock cap for the extraction sidecar (which runs the model —
 * "seconds to minutes" on a CPU model). An unbounded popen/pclose wedges the
 * drain thread forever when the sidecar hangs. */
#define CCU_SIDECAR_TIMEOUT_S 300
/* A job left in 'running' longer than this lease was orphaned (worker crash/
 * restart or a wedged call) — comfortably above the sidecar cap above. */
#define CCU_STALE_LEASE "-15 minutes"
/* Reclaim runs at most this often (throttled; the drain calls the entry per job). */
#define CCU_RECLAIM_EVERY_S 60

typedef struct
{
   int64_t job_id;
   char project[256];
   char file_path[1024];
   char symbol[256];
   char kind[64];
   int line;
   int attempts;
   /* Derived from the function body before it is freed, stashed for the
    * code_unit artifact payload (signature_vec / body_vec source). */
   char signature[256];
   char body_excerpt[1536];
} ccu_job_t;

/* ── DB helpers ──────────────────────────────────────────────────────────── */

static int ccu_claim_job(ccu_job_t *out)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "UPDATE kb_code_unit_jobs"
                            " SET status='running',"
                            "     claimed_by='kb.curator.drain',"
                            "     claimed_at=pg_now_text(),"
                            "     attempts=attempts+1,"
                            "     updated_at=pg_now_text()"
                            " WHERE id=("
                            "   SELECT id FROM kb_code_unit_jobs"
                            "   WHERE status='pending'"
                            "   ORDER BY id LIMIT 1 FOR UPDATE SKIP LOCKED"
                            " )"
                            " RETURNING id, project, file_path, symbol, kind, line, attempts";

   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->job_id = aimee_pg_column_int64(st, 0);
      const char *proj = aimee_pg_column_text(st, 1);
      const char *fp = aimee_pg_column_text(st, 2);
      const char *sym = aimee_pg_column_text(st, 3);
      const char *knd = aimee_pg_column_text(st, 4);
      snprintf(out->project, sizeof(out->project), "%s", proj ? proj : "");
      snprintf(out->file_path, sizeof(out->file_path), "%s", fp ? fp : "");
      snprintf(out->symbol, sizeof(out->symbol), "%s", sym ? sym : "");
      snprintf(out->kind, sizeof(out->kind), "%s", knd ? knd : "function");
      out->line = aimee_pg_column_int(st, 5);
      out->attempts = aimee_pg_column_int(st, 6);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

static void ccu_mark_done(int64_t job_id)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "UPDATE kb_code_unit_jobs SET status='done', updated_at=pg_now_text() WHERE id=?1",
       err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int64(st, "?1", job_id);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

static void ccu_mark_retry_or_fail(int64_t job_id, int attempts, int max_attempts,
                                   const char *error_msg)
{
   void *conn = db2_conn();
   if (!conn)
      return;

   const char *new_status = (attempts >= max_attempts) ? "failed" : "pending";
   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "UPDATE kb_code_unit_jobs"
                                          " SET status=?1, last_error=?2, updated_at=pg_now_text()"
                                          " WHERE id=?3",
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

/* Reclaim code-unit jobs orphaned in 'running'. The claim only ever selects
 * status='pending', so a job whose worker crashed/restarted or whose sidecar
 * wedged stays 'running' forever — lost work, and (until the fix above) a
 * permanently shrunk drain. Reset rows older than the lease back to 'pending'
 * so they retry, or to 'failed' once attempts are exhausted so a poison job
 * cannot loop. attempts is preserved (it was incremented at claim time).
 * Throttled to at most once per CCU_RECLAIM_EVERY_S since the drain calls the
 * entry point once per job. */
static void ccu_reclaim_stale_running(int max_attempts)
{
   static time_t last_run = 0;
   time_t now = time(NULL);
   if (last_run != 0 && now - last_run < CCU_RECLAIM_EVERY_S)
      return;
   last_run = now;

   if (max_attempts < 1)
      max_attempts = 1;

   void *conn = db2_conn();
   if (!conn)
      return;
   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "UPDATE kb_code_unit_jobs"
       " SET status = CASE WHEN attempts >= ?1 THEN 'failed' ELSE 'pending' END,"
       "     claimed_by = '', claimed_at = '',"
       "     last_error = CASE WHEN attempts >= ?1"
       "                       THEN 'stale running lease reclaimed after max attempts'"
       "                       ELSE last_error END,"
       "     updated_at = pg_now_text()"
       " WHERE status = 'running' AND claimed_at <> '' AND claimed_at < pg_now_text(?2)",
       err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_int(st, "?1", max_attempts);
   aimee_pg_bind_text(st, "?2", CCU_STALE_LEASE);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

/* ── Body extraction ─────────────────────────────────────────────────────── */

/* Resolve a (possibly relative) file_path to an absolute on-disk path. The
 * structural index stores project-relative paths (files.path), so a code-unit
 * job's file_path must be joined onto the project's recorded root
 * (projects.root) before it can be opened — the drain's CWD is not the project
 * root. An already-absolute path is used as-is; an unknown root falls back to
 * the bare path. */
static void ccu_resolve_path(const char *project, const char *file_path, char *out, size_t out_len)
{
   if (file_path && file_path[0] == '/')
   {
      snprintf(out, out_len, "%s", file_path);
      return;
   }
   char root[1024] = "";
   void *conn = db2_conn();
   if (conn && project && project[0])
   {
      char err[CCU_ERRBUF] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(
          conn, "SELECT root FROM projects WHERE name=?1 LIMIT 1", err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_text(st, "?1", project);
         if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *r = aimee_pg_column_text(st, 0);
            if (r && r[0])
               snprintf(root, sizeof(root), "%s", r);
         }
         aimee_pg_finalize(st);
      }
   }
   if (root[0])
      snprintf(out, out_len, "%s/%s", root, file_path ? file_path : "");
   else
      snprintf(out, out_len, "%s", file_path ? file_path : "");
}

/* Slice CCU_BODY_LINES lines starting at `line` (1-based) out of an in-memory
 * file `content`. Returns malloc'd string (caller frees) or NULL on OOM. */
static char *ccu_slice_lines(const char *content, int line)
{
   int start = line > 1 ? line - 1 : 0;
   int end = start + CCU_BODY_LINES;
   char *body = malloc(strlen(content) + 1); /* a subset never exceeds the whole */
   if (!body)
      return NULL;
   size_t total = 0;
   int cur = 0;
   const char *p = content;
   while (*p)
   {
      const char *nl = strchr(p, '\n');
      size_t n = nl ? (size_t)(nl - p) + 1 : strlen(p);
      if (cur >= start && cur < end)
      {
         memcpy(body + total, p, n);
         total += n;
      }
      cur++;
      if (!nl || cur >= end)
         break;
      p += n;
   }
   body[total] = '\0';
   return body;
}

/* Read the code-unit body from the file's content stored in DB2 (file_contents),
 * keyed by project + project-relative path. This is the primary source: the
 * curator drain runs server-side, where thin-client-ingested files do not exist
 * on disk (projects.root points at the client's path) — but ingest pushes the
 * whole file content into file_contents. Returns malloc'd string (caller frees),
 * or NULL when no stored content exists (caller falls back to an on-disk read for
 * local deployments). */
static char *ccu_read_body_db2(const char *project, const char *file_path, int line)
{
   void *conn = db2_conn();
   if (!conn || !project || !project[0] || !file_path || !file_path[0])
      return NULL;
   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT fc.content FROM file_contents fc"
                                          " JOIN files f ON f.id = fc.file_id"
                                          " JOIN projects p ON p.id = f.project_id"
                                          " WHERE p.name = ?1 AND f.path = ?2 LIMIT 1",
                                          err, sizeof(err));
   if (!st)
      return NULL;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   char *content = NULL;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *c = aimee_pg_column_text(st, 0);
      if (c && c[0])
         content = strdup(c);
   }
   aimee_pg_finalize(st);
   if (!content)
      return NULL;
   char *body = ccu_slice_lines(content, line);
   free(content);
   return body;
}

/* Read CCU_BODY_LINES lines starting at line (1-based) from file_path.
 * Returns malloc'd string (caller frees) or NULL on error. */
static char *ccu_read_body(const char *file_path, int line, char *errbuf, size_t errlen)
{
   FILE *fp = fopen(file_path, "r");
   if (!fp)
   {
      snprintf(errbuf, errlen, "cannot open %s", file_path);
      return NULL;
   }

   /* Allocate generous body buffer */
   size_t bufsz = 64 * 1024;
   char *body = malloc(bufsz);
   if (!body)
   {
      fclose(fp);
      snprintf(errbuf, errlen, "out of memory");
      return NULL;
   }

   /* line is 1-based; skip lines before it */
   int start = line > 1 ? line - 1 : 0; /* 0-based index of first line to include */
   int end = start + CCU_BODY_LINES;

   char linebuf[4096];
   size_t total = 0;
   int cur = 0;

   while (fgets(linebuf, sizeof(linebuf), fp))
   {
      if (cur >= start && cur < end)
      {
         size_t n = strlen(linebuf);
         if (total + n + 1 >= bufsz)
         {
            /* Grow buffer */
            bufsz *= 2;
            char *nb = realloc(body, bufsz);
            if (!nb)
               break;
            body = nb;
         }
         memcpy(body + total, linebuf, n);
         total += n;
      }
      cur++;
      if (cur >= end)
         break;
   }
   fclose(fp);

   body[total] = '\0';
   return body;
}

/* ── Sidecar invocation ──────────────────────────────────────────────────── */

static char *ccu_invoke_sidecar(const char *cmd, const char *json_input, char *errbuf,
                                size_t errlen)
{
   /* Honour TMPDIR like the rest of the tree (see code_collect.c): a hardcoded
    * /tmp gives an operator no way to move the spool off a full or slow
    * filesystem. */
   const char *tmpdir = getenv("TMPDIR");
   if (!tmpdir || !tmpdir[0])
      tmpdir = "/tmp";
   char tmppath[256];
   snprintf(tmppath, sizeof(tmppath), "%s/aimee_ccu_XXXXXX", tmpdir);
   int fd = mkstemp(tmppath);
   if (fd < 0)
   {
      /* Carry errno: ENOSPC (full spool), EMFILE (fd exhaustion) and EACCES are
       * different incidents with different fixes, and last_error is the only
       * forensic record a failed job leaves behind. */
      snprintf(errbuf, errlen, "mkstemp failed for %s: %s", tmppath, strerror(errno));
      return NULL;
   }

   size_t inlen = strlen(json_input);
   if (write(fd, json_input, inlen) != (ssize_t)inlen)
   {
      close(fd);
      unlink(tmppath);
      snprintf(errbuf, errlen, "write to tmpfile failed");
      return NULL;
   }
   close(fd);

   char full_cmd[1024];
   /* Bound the sidecar with coreutils `timeout` (present on the kb image):
    * SIGTERM at the ceiling, SIGKILL 10s later if it ignores TERM. Without this
    * a hung sidecar wedges the drain thread forever via pclose(); on timeout
    * pclose() returns non-zero and the caller retries/fails the job instead.
    * Run the configured command under `sh -c` INSIDE timeout so it keeps the
    * shell semantics popen() gave it (env-var prefixes like `FOO=bar python …`,
    * builtins, operators) — passing it as timeout's own argv would break those
    * and let compound commands escape the bound. `cmd` is trusted config. */
   snprintf(full_cmd, sizeof(full_cmd), "timeout -k 10 %d sh -c \"%s\" < %s", CCU_SIDECAR_TIMEOUT_S,
            cmd, tmppath);

   FILE *fp = popen(full_cmd, "r");
   if (!fp)
   {
      unlink(tmppath);
      snprintf(errbuf, errlen, "popen failed for: %s", full_cmd);
      return NULL;
   }

   char *out = malloc(CCU_OUTBUF);
   if (!out)
   {
      pclose(fp);
      unlink(tmppath);
      snprintf(errbuf, errlen, "out of memory");
      return NULL;
   }

   size_t total = 0;
   size_t n;
   while ((n = fread(out + total, 1, CCU_OUTBUF - total - 1, fp)) > 0)
   {
      total += n;
      if (total >= CCU_OUTBUF - 1)
         break;
   }
   out[total] = '\0';
   int rc = pclose(fp);
   unlink(tmppath);

   if (rc != 0)
   {
      kb_curator_describe_wait_status(rc, CCU_SIDECAR_TIMEOUT_S, errbuf, errlen);
      free(out);
      return NULL;
   }

   return out;
}

/* Sidecar command resolution is shared with the doc extract stage; see
 * kb_curator_resolve_sidecar_command() in kb_curator_extract.c. */

/* ── Structural grounding gate ─────────────────────────────────────────────
 * Reject a code_unit whose extracted payload claims the symbol has no side
 * effects when the structural call graph (code_calls) shows it calls a known
 * side-effecting / system-call function. Mirrors the pure decision in
 * kb_curator_grounding_contradicts(), streaming callee rows so we never have
 * to buffer the whole edge set. Returns 1 (and fills reason_out with the
 * offending callee) on contradiction, else 0. Conservative: if the payload
 * does not claim "no side effects", or no structural edges are known, it
 * returns 0 and the artifact commits normally. */
static int ccu_payload_contradicts_structure(const ccu_job_t *job, const cJSON *payload,
                                             char *reason_out, size_t reason_len)
{
   if (reason_out && reason_len > 0)
      reason_out[0] = '\0';

   if (!kb_curator_payload_claims_no_side_effects(payload))
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT cc.callee"
                            " FROM code_calls cc"
                            " JOIN files f ON cc.file_id = f.id"
                            " JOIN projects p ON f.project_id = p.id"
                            " WHERE p.name = ?1 AND f.path = ?2 AND cc.caller = ?3";

   char err[CCU_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   aimee_pg_bind_text(st, "?1", job->project);
   aimee_pg_bind_text(st, "?2", job->file_path);
   aimee_pg_bind_text(st, "?3", job->symbol);

   int contradicted = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *callee = aimee_pg_column_text(st, 0);
      if (callee && kb_curator_callee_is_side_effecting(callee))
      {
         if (reason_out && reason_len > 0)
            snprintf(reason_out, reason_len, "%s", callee);
         contradicted = 1;
         break;
      }
   }
   aimee_pg_finalize(st);
   return contradicted;
}

/* ── Artifact write ──────────────────────────────────────────────────────── */

static int ccu_write_artifacts(const ccu_job_t *job, cJSON *artifacts_arr)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   char err[CCU_ERRBUF] = "";
   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int n = cJSON_GetArraySize(artifacts_arr);
   int committed = 0;
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

      /* Enrich code_unit payloads with the columns the curator_code_unit_vectors
       * indexer needs (file_path / def_kind / signature / body_excerpt), unless
       * the sidecar already provided them. */
      if (cJSON_IsObject(payload_j) && strcmp(kind_j->valuestring, "code_unit") == 0)
      {
         cJSON *pj = (cJSON *)payload_j;
         if (!cJSON_GetObjectItemCaseSensitive(pj, "file_path"))
            cJSON_AddStringToObject(pj, "file_path", job->file_path);
         if (!cJSON_GetObjectItemCaseSensitive(pj, "symbol"))
            cJSON_AddStringToObject(pj, "symbol", job->symbol);
         if (!cJSON_GetObjectItemCaseSensitive(pj, "signature"))
            cJSON_AddStringToObject(pj, "signature", job->signature);
         if (!cJSON_GetObjectItemCaseSensitive(pj, "def_kind"))
            cJSON_AddStringToObject(pj, "def_kind", job->kind[0] ? job->kind : "function");
         if (!cJSON_GetObjectItemCaseSensitive(pj, "body_excerpt"))
            cJSON_AddStringToObject(pj, "body_excerpt", job->body_excerpt);
      }

      char *payload_str = payload_j ? cJSON_PrintUnformatted(payload_j) : NULL;

      char id_buf[64];
      db2_artifact_gen_id(id_buf, sizeof(id_buf));

      int wrc = db2_artifact_write(id_buf, kind_j->valuestring, "proposed", "project", job->project,
                                   "kb.curator.extract_code", confidence,
                                   payload_str ? payload_str : "{}");
      free(payload_str);

      if (wrc != 0)
      {
         aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
         return -1;
      }

      /* Structural-grounding gate: reject pre-commit if the payload claims no
       * side effects but the call graph proves otherwise (deep-curator AC#7). */
      char ground_reason[128] = "";
      if (ccu_payload_contradicts_structure(job, payload_j, ground_reason, sizeof(ground_reason)))
      {
         char before_json[512];
         snprintf(before_json, sizeof(before_json),
                  "{\"gate\":\"structural_grounding\",\"symbol\":\"%s\",\"callee\":\"%s\","
                  "\"detail\":\"claimed no side effects but calls side-effecting %s\"}",
                  job->symbol, ground_reason, ground_reason);
         db2_artifact_reject(id_buf, "structural_grounding", "side_effects", ground_reason,
                             before_json);
         aimee_log(LOG_WARN, "kb.curator.extract_code",
                   "rejected code_unit for symbol '%s' (job %lld): claims no side effects but "
                   "calls side-effecting '%s'",
                   job->symbol, (long long)job->job_id, ground_reason);
         continue;
      }

      db2_artifact_cite(id_buf, "kb_file", job->file_path);
      committed++;
   }

   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;

   return committed;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

int kb_curator_extract_code_unit_one(const kb_curator_extract_opts_t *opts)
{
   ccu_job_t job;
   memset(&job, 0, sizeof(job));

   /* Recover jobs orphaned in 'running' before claiming fresh work, so a crash/
    * restart or a previously-wedged sidecar cannot permanently strand them. */
   ccu_reclaim_stale_running(opts ? opts->max_attempts : 1);

   if (!ccu_claim_job(&job))
      return 0; /* queue empty */

   aimee_log(LOG_DEBUG, "kb.curator.extract_code",
             "claimed extract_code_unit job %lld symbol '%s' in '%s'", (long long)job.job_id,
             job.symbol, job.file_path);

   char body_err[CCU_ERRBUF] = "";
   /* Primary: the file content stored in DB2 at ingest (works server-side for
    * thin-client-ingested files). Fallback: an on-disk read for local
    * deployments whose file_contents may be empty. */
   char *body = ccu_read_body_db2(job.project, job.file_path, job.line);
   if (!body)
   {
      char abs_path[2048];
      ccu_resolve_path(job.project, job.file_path, abs_path, sizeof(abs_path));
      body = ccu_read_body(abs_path, job.line, body_err, sizeof(body_err));
   }
   if (!body)
   {
      if (!body_err[0])
         snprintf(body_err, sizeof(body_err), "no stored content and cannot open %s",
                  job.file_path);
      aimee_log(LOG_WARN, "kb.curator.extract_code", "cannot read body for job %lld: %s",
                (long long)job.job_id, body_err);
      ccu_mark_retry_or_fail(job.job_id, opts->max_attempts, opts->max_attempts, body_err);
      return 1;
   }

   cJSON *req = cJSON_CreateObject();
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", "extract_code_unit");
   cJSON *inp = cJSON_AddObjectToObject(req, "input");
   cJSON_AddStringToObject(inp, "file_path", job.file_path);
   cJSON_AddStringToObject(inp, "symbol", job.symbol);
   cJSON_AddStringToObject(inp, "kind", job.kind);
   cJSON_AddNumberToObject(inp, "line", job.line);
   cJSON_AddStringToObject(inp, "body", body);
   /* Stash a bounded signature (first non-blank line) + body excerpt for the
    * code_unit artifact payload before the body buffer is released. */
   {
      const char *p = body;
      while (*p == '\n' || *p == '\r' || *p == ' ' || *p == '\t')
         p++;
      size_t siglen = 0;
      while (p[siglen] && p[siglen] != '\n' && siglen < sizeof(job.signature) - 1)
         siglen++;
      memcpy(job.signature, p, siglen);
      job.signature[siglen] = '\0';
      snprintf(job.body_excerpt, sizeof(job.body_excerpt), "%s", body);
   }
   free(body);
   cJSON *conf = cJSON_AddObjectToObject(req, "config");
   cJSON_AddNumberToObject(conf, "max_tokens", opts->max_tokens > 0 ? opts->max_tokens : 2048);

   char *req_str = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!req_str)
   {
      ccu_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts,
                             "failed to serialize request JSON");
      return 1;
   }

   char cmd[768];
   kb_curator_resolve_sidecar_command(opts, cmd, sizeof(cmd));

   aimee_log(LOG_INFO, "kb.curator.extract_code", "invoking sidecar for symbol '%s' (job %lld): %s",
             job.symbol, (long long)job.job_id, cmd);

   /* Return our pool connection before the slow sidecar/LLM call — extraction on
    * a CPU model runs seconds to minutes, and holding a lease across it trips the
    * pool's stuck-lease ceiling (300s) and permanently shrinks the pool. All
    * remaining DB work (grounding, artifact write, mark_done) re-acquires lazily
    * via db2_conn() after the call returns. Safe: we are at lease depth 0 (the
    * drain uses db2_lease_release_idle, not an explicit begin/end scope), and the
    * body was already read from DB2 above. */
   db2_lease_release_idle();

   char sidecar_err[512] = "";
   char *resp_str = ccu_invoke_sidecar(cmd, req_str, sidecar_err, sizeof(sidecar_err));
   free(req_str);

   if (!resp_str)
   {
      aimee_log(LOG_WARN, "kb.curator.extract_code",
                "sidecar failed for job %lld (attempt %d/%d): %s", (long long)job.job_id,
                job.attempts, opts->max_attempts, sidecar_err);
      ccu_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, sidecar_err);
      return 1;
   }

   cJSON *resp = cJSON_Parse(resp_str);
   free(resp_str);

   if (!resp)
   {
      aimee_log(LOG_WARN, "kb.curator.extract_code", "sidecar returned non-JSON for job %lld",
                (long long)job.job_id);
      ccu_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts,
                             "sidecar returned non-JSON");
      return 1;
   }

   const cJSON *status_j = cJSON_GetObjectItemCaseSensitive(resp, "status");
   if (!cJSON_IsString(status_j) || strcmp(status_j->valuestring, "ok") != 0)
   {
      const cJSON *err_j = cJSON_GetObjectItemCaseSensitive(resp, "error");
      const char *errmsg = cJSON_IsString(err_j) ? err_j->valuestring : "sidecar status != ok";
      aimee_log(LOG_WARN, "kb.curator.extract_code", "sidecar error for job %lld: %s",
                (long long)job.job_id, errmsg);
      ccu_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, errmsg);
      cJSON_Delete(resp);
      return 1;
   }

   cJSON *artifacts = cJSON_GetObjectItemCaseSensitive(resp, "artifacts");
   int n_written = 0;
   if (cJSON_IsArray(artifacts))
      n_written = ccu_write_artifacts(&job, artifacts);

   cJSON_Delete(resp);

   if (n_written < 0)
   {
      aimee_log(LOG_WARN, "kb.curator.extract_code", "artifact write failed for job %lld",
                (long long)job.job_id);
      ccu_mark_retry_or_fail(job.job_id, job.attempts, opts->max_attempts, "artifact write failed");
      return 1;
   }

   aimee_log(LOG_INFO, "kb.curator.extract_code", "wrote %d artifact(s) for symbol '%s' (job %lld)",
             n_written, job.symbol, (long long)job.job_id);
   ccu_mark_done(job.job_id);
   return 1;
}
