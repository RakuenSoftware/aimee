/* kb_payload.c: DB2 payload builder for vector kb chunks.
 * Postgres via libpq. Callers build payload JSON before handing it to
 * the pgvector upsert helpers. */

#include "kb_payload.h"
#include "aimee.h"
#include "artifacts.h"
#include "config.h"
#include "memory.h"
#include "pgvec_transport.h"

#include "db_postgres.h"
#include "cJSON.h"
#include "db2_internal.h"
#include "../headers/log.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KBP_ERRBUF 256

char *db2_kb_build_document_payload(int64_t doc_id)
{
   if (doc_id <= 0)
      return NULL;
   void *conn = db2_conn();
   if (!conn)
      return NULL;

   static const char *sql =
       "SELECT project, file_path, heading_path, line_start, line_end, file_hash, chunk_index"
       " FROM kb_documents WHERE id = ?1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return NULL;
   aimee_pg_bind_int64(st, "?1", doc_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return NULL;
   }

   const char *project = aimee_pg_column_text(st, 0);
   const char *file_path = aimee_pg_column_text(st, 1);
   const char *heading_path = aimee_pg_column_text(st, 2);
   int line_start = aimee_pg_column_int(st, 3);
   int line_end = aimee_pg_column_int(st, 4);
   const char *file_hash = aimee_pg_column_text(st, 5);
   int chunk_index = aimee_pg_column_int(st, 6);

   cJSON *payload = cJSON_CreateObject();
   cJSON_AddStringToObject(payload, "record_type", "kb_chunk");
   cJSON_AddNumberToObject(payload, "document_id", (double)doc_id);
   cJSON_AddStringToObject(payload, "project", project ? project : "");
   cJSON_AddStringToObject(payload, "file_path", file_path ? file_path : "");
   cJSON_AddStringToObject(payload, "heading_path", heading_path ? heading_path : "");
   cJSON_AddNumberToObject(payload, "line_start", line_start);
   cJSON_AddNumberToObject(payload, "line_end", line_end);
   cJSON_AddNumberToObject(payload, "chunk_index", chunk_index);
   if (file_hash && file_hash[0])
      cJSON_AddStringToObject(payload, "file_hash", file_hash);
   char *payload_json = cJSON_PrintUnformatted(payload);
   cJSON_Delete(payload);
   aimee_pg_finalize(st);
   return payload_json;
}

int db2_kb_document_fetch(int64_t id, const char *project, db2_kb_document_row_t *out)
{
   if (!out)
      return 0;
   memset(out, 0, sizeof(*out));
   if (id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* id is the kb_documents primary key (globally unique), so an absent project
    * fetches by id alone — this lets a project-less (whole-corpus) kb/search
    * resolve the rows pgvec_kb_search returns across all projects. A named
    * project still scopes the lookup. */
   int has_project = (project && project[0]);
   const char *sql = has_project ? "SELECT id, file_path, file_hash, heading_path, line_start, "
                                   "line_end, content, doc_kind"
                                   " FROM kb_documents WHERE id = ?1 AND project = ?2"
                                 : "SELECT id, file_path, file_hash, heading_path, line_start, "
                                   "line_end, content, doc_kind"
                                   " FROM kb_documents WHERE id = ?1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", id);
   if (has_project)
      aimee_pg_bind_text(st, "?2", project);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out->id = aimee_pg_column_int64(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *fh = aimee_pg_column_text(st, 2);
      const char *hp = aimee_pg_column_text(st, 3);
      const char *ct = aimee_pg_column_text(st, 6);
      const char *dk = aimee_pg_column_text(st, 7);
      snprintf(out->file_path, sizeof(out->file_path), "%s", fp ? fp : "");
      snprintf(out->file_hash, sizeof(out->file_hash), "%s", fh ? fh : "");
      snprintf(out->heading_path, sizeof(out->heading_path), "%s", hp ? hp : "");
      out->line_start = aimee_pg_column_int(st, 4);
      out->line_end = aimee_pg_column_int(st, 5);
      snprintf(out->content, sizeof(out->content), "%s", ct ? ct : "");
      snprintf(out->doc_kind, sizeof(out->doc_kind), "%s", dk ? dk : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_kb_documents_list_convention_candidates(db2_kb_convention_row_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   /* doc_kind <> 'pdf': a PDF whose file_path happens to match a convention pattern
    * (e.g. docs/adr/0007.pdf) must not have its content pulled into agent-facing
    * conventions — PDF content stays behind the access-gated search_chunks tool. */
   static const char *sql = "SELECT project, file_path, heading_path, content FROM kb_documents"
                            " WHERE doc_kind <> 'pdf' AND ("
                            "       file_path LIKE '%CONTRIBUTING%'"
                            "    OR file_path LIKE '%AGENTS.md'"
                            "    OR file_path LIKE '%STYLE%'"
                            "    OR file_path LIKE '%CODING%'"
                            "    OR file_path LIKE '%.aimee-rules%'"
                            "    OR file_path LIKE '%.aimee/rules.md'"
                            "    OR file_path LIKE '%.aimee/context.md'"
                            "    OR file_path LIKE '%/adr/%')"
                            " ORDER BY project, file_path, chunk_index"
                            " LIMIT ?1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int(st, "?1", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      const char *p = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *hp = aimee_pg_column_text(st, 2);
      const char *ct = aimee_pg_column_text(st, 3);
      snprintf(out[n].project, sizeof(out[n].project), "%s", p ? p : "");
      snprintf(out[n].file_path, sizeof(out[n].file_path), "%s", fp ? fp : "");
      snprintf(out[n].heading_path, sizeof(out[n].heading_path), "%s", hp ? hp : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", ct ? ct : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_documents_get_stored_hash(const char *project, const char *file_path, char *out,
                                     size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   if (!project || !*project || !file_path || !*file_path)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "SELECT file_hash FROM kb_documents WHERE project = ?1 AND file_path = ?2 LIMIT 1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *h = aimee_pg_column_text(st, 0);
      if (h)
      {
         snprintf(out, out_len, "%s", h);
         rc = 0;
      }
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_kb_documents_hash_exists(const char *project, const char *file_hash, char *sample_path,
                                 size_t sample_path_len)
{
   if (sample_path && sample_path_len)
      sample_path[0] = '\0';
   if (!project || !*project || !file_hash || !*file_hash)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT file_path FROM kb_documents"
                            " WHERE project = ?1 AND file_hash = ?2"
                            " UNION"
                            " SELECT file_path FROM kb_file_index"
                            " WHERE project = ?1 AND file_hash = ?2"
                            " LIMIT 1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_hash);

   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *fp = aimee_pg_column_text(st, 0);
      if (sample_path && sample_path_len && fp)
         snprintf(sample_path, sample_path_len, "%s", fp);
      found = 1;
   }
   aimee_pg_finalize(st);
   return found;
}

int db2_kb_documents_hll_sources_for_hash(const char *project, const char *file_hash,
                                          sketch_hll_t *out)
{
   if (!out)
      return -1;
   sketch_hll_init(out);
   if (!project || !*project || !file_hash || !*file_hash)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "SELECT file_path FROM kb_documents"
                            " WHERE project = ?1 AND file_hash = ?2"
                            " UNION"
                            " SELECT file_path FROM kb_file_index"
                            " WHERE project = ?1 AND file_hash = ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_hash);

   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *fp = aimee_pg_column_text(st, 0);
      if (fp && fp[0])
      {
         sketch_hll_add_hash(out, sketch_fnv1a(fp, strlen(fp)));
         n++;
      }
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_async_enqueue(const char *kind, int64_t document_id, const char *project)
{
   if (!kind || !*kind || document_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql = "INSERT INTO kb_async_jobs"
                            " (kind, document_id, project, status, updated_at)"
                            " VALUES (?1, ?2, ?3, 'pending', pg_now_text())"
                            " ON CONFLICT (kind, document_id) DO NOTHING";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", kind);
   aimee_pg_bind_int64(st, "?2", document_id);
   aimee_pg_bind_text(st, "?3", project ? project : "");
   int rc = (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE) ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

/* Version-bump (prompt_version): re-extract every document by re-enqueuing an
 * extract_doc job for it (existing done jobs are re-armed to pending). Returns
 * the number of documents re-enqueued. */
static void kbp_exec(void *conn, const char *sql)
{
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_curator_reenqueue_extract_all(void)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Two cross-backend statements (the sqlite test shim rejects ON CONFLICT DO
    * UPDATE inside INSERT...SELECT): enqueue any document without an extract_doc
    * job, then re-arm every existing extract_doc job back to pending. */
   kbp_exec(conn, "INSERT INTO kb_async_jobs (kind, document_id, project, status)"
                  " SELECT 'extract_doc', d.id, d.project, 'pending' FROM kb_documents d"
                  " WHERE NOT EXISTS (SELECT 1 FROM kb_async_jobs j"
                  "   WHERE j.kind = 'extract_doc' AND j.document_id = d.id)");
   kbp_exec(conn, "UPDATE kb_async_jobs SET status = 'pending'"
                  " WHERE kind = 'extract_doc' AND status <> 'pending'");

   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM kb_async_jobs WHERE kind = 'extract_doc'", err, sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_documents_list_chunk_ids_for_file(const char *project, const char *file_path,
                                             int64_t *out, int max)
{
   if (!project || !*project || !file_path || !*file_path || !out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT id FROM kb_documents WHERE project = ?1 AND file_path = ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      out[n++] = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* Invalidation: mark every curator artifact citing any chunk of (project,
 * file_path) as stale, so a changed/removed source doc invalidates its derived
 * artifacts (which are then re-extracted by the post-ingest queue). Returns the
 * number of artifacts marked stale. Must be called before the chunks are
 * deleted so their ids are still resolvable. */
int db2_curator_invalidate_doc(const char *project, const char *file_path)
{
   if (!project || !*project || !file_path || !*file_path)
      return 0;
   int64_t ids[1024];
   int n = db2_kb_documents_list_chunk_ids_for_file(project, file_path, ids,
                                                    (int)(sizeof(ids) / sizeof(ids[0])));
   int total = 0;
   for (int i = 0; i < n; i++)
   {
      char id_str[32];
      snprintf(id_str, sizeof(id_str), "%lld", (long long)ids[i]);
      int m = db2_artifact_invalidate_citing("kb_document", id_str, 0, 0);
      if (m > 0)
         total += m;
   }
   if (total > 0)
      db2_curator_invalidation_record("kb_file", file_path, total);
   return total;
}

void db2_curator_invalidation_record(const char *source_kind, const char *source_id,
                                     int artifacts_stale)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   static const char *sql = "INSERT INTO curator_invalidation_events"
                            " (source_kind, source_id, artifacts_stale) VALUES (?1, ?2, ?3)";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", source_kind ? source_kind : "");
   aimee_pg_bind_text(st, "?2", source_id ? source_id : "");
   aimee_pg_bind_int(st, "?3", artifacts_stale);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_curator_invalidations_since(int64_t since_id, db2_curator_invalidation_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT id, source_kind, source_id, artifacts_stale, created_at"
       " FROM curator_invalidation_events WHERE id > ?1 ORDER BY id ASC LIMIT ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", since_id);
   aimee_pg_bind_int(st, "?2", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      out[n].id = aimee_pg_column_int64(st, 0);
      const char *sk = aimee_pg_column_text(st, 1);
      const char *si = aimee_pg_column_text(st, 2);
      snprintf(out[n].source_kind, sizeof(out[n].source_kind), "%s", sk ? sk : "");
      snprintf(out[n].source_id, sizeof(out[n].source_id), "%s", si ? si : "");
      out[n].artifacts_stale = aimee_pg_column_int(st, 3);
      const char *ca = aimee_pg_column_text(st, 4);
      snprintf(out[n].created_at, sizeof(out[n].created_at), "%s", ca ? ca : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_kb_documents_delete_for_file(const char *project, const char *file_path)
{
   if (!project || !*project || !file_path || !*file_path)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   static const char *sql = "DELETE FROM kb_documents WHERE project = ?1 AND file_path = ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int64_t db2_kb_documents_insert_chunk(const char *project, const char *file_path,
                                      const char *file_hash, int chunk_index,
                                      const char *heading_path, int line_start, int line_end,
                                      const char *content, int token_count)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO kb_documents"
       " (project, file_path, file_hash, chunk_index, heading_path, line_start, line_end,"
       "  content, token_count, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, pg_now_text())"
       " RETURNING id";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project ? project : "");
   aimee_pg_bind_text(st, "?2", file_path ? file_path : "");
   aimee_pg_bind_text(st, "?3", file_hash ? file_hash : "");
   aimee_pg_bind_int(st, "?4", chunk_index);
   aimee_pg_bind_text(st, "?5", heading_path ? heading_path : "");
   aimee_pg_bind_int(st, "?6", line_start);
   aimee_pg_bind_int(st, "?7", line_end);
   aimee_pg_bind_text(st, "?8", content ? content : "");
   aimee_pg_bind_int(st, "?9", token_count);
   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return new_id;
}

void db2_kb_documents_link_neighbours(int64_t doc_id, int64_t prev_id)
{
   if (prev_id <= 0 || doc_id <= 0)
      return;
   void *conn = db2_conn();
   if (!conn)
      return;

   char err[KBP_ERRBUF] = "";
   {
      static const char *sql = "UPDATE kb_documents SET prev_chunk_id = ?1 WHERE id = ?2";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", prev_id);
         aimee_pg_bind_int64(st, "?2", doc_id);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }
   {
      static const char *sql = "UPDATE kb_documents SET next_chunk_id = ?1 WHERE id = ?2";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", doc_id);
         aimee_pg_bind_int64(st, "?2", prev_id);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
   }
}

/* structured-pdf Phase 1: like db2_kb_documents_insert_chunk but also stamps the
 * PDF-specific columns in the same INSERT (doc_kind='pdf', the caller's
 * chunk_strategy — 'heading' or the 'page' fallback — and the page_start/page_end
 * span). Kept separate from the 9-arg insert so the markdown path is untouched. */
int64_t db2_kb_documents_insert_chunk_pdf(const char *project, const char *file_path,
                                          const char *file_hash, int chunk_index,
                                          const char *heading_path, int line_start, int line_end,
                                          const char *content, int token_count,
                                          const char *chunk_strategy, int page_start, int page_end,
                                          const char *sensitivity_class,
                                          const char *quarantine_state)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;

   static const char *sql =
       "INSERT INTO kb_documents"
       " (project, file_path, file_hash, chunk_index, heading_path, line_start, line_end,"
       "  content, token_count, doc_kind, chunk_strategy, page_start, page_end,"
       "  sensitivity_class, quarantine_state, updated_at)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, 'pdf', ?10, ?11, ?12, ?13, ?14, pg_now_text())"
       " RETURNING id";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project ? project : "");
   aimee_pg_bind_text(st, "?2", file_path ? file_path : "");
   aimee_pg_bind_text(st, "?3", file_hash ? file_hash : "");
   aimee_pg_bind_int(st, "?4", chunk_index);
   aimee_pg_bind_text(st, "?5", heading_path ? heading_path : "");
   aimee_pg_bind_int(st, "?6", line_start);
   aimee_pg_bind_int(st, "?7", line_end);
   aimee_pg_bind_text(st, "?8", content ? content : "");
   aimee_pg_bind_int(st, "?9", token_count);
   aimee_pg_bind_text(st, "?10", chunk_strategy ? chunk_strategy : "heading");
   aimee_pg_bind_int(st, "?11", page_start);
   aimee_pg_bind_int(st, "?12", page_end);
   aimee_pg_bind_text(st, "?13", sensitivity_class ? sensitivity_class : "");
   aimee_pg_bind_text(st, "?14", quarantine_state ? quarantine_state : "");
   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return new_id;
}

/* Thin transaction wrappers (work on both Postgres and the sqlite test shim) so a
 * multi-statement write — e.g. the structured-PDF delete-then-insert re-ingest — is
 * all-or-nothing. Return 0 on success, <0 on error. */
int db2_kb_txn_begin(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[KBP_ERRBUF] = "";
   return aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) == 0 ? 0 : -1;
}

int db2_kb_txn_commit(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[KBP_ERRBUF] = "";
   return aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0 ? 0 : -1;
}

void db2_kb_txn_rollback(void)
{
   void *conn = db2_conn();
   if (!conn)
      return;
   char err[KBP_ERRBUF] = "";
   (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
}

/* structured-pdf Phase 1: insert one per-line coordinate region for a chunk. bbox
 * is already normalized to [0,1] (top-left origin, per page) by the caller. */
int64_t db2_kb_doc_regions_insert(int64_t chunk_id, const char *document_key, int page_no,
                                  double x0, double y0, double x1, double y1, const char *quote,
                                  int line_index, const char *content_type,
                                  const char *sensitivity_class)
{
   void *conn = db2_conn();
   if (!conn || chunk_id <= 0)
      return -1;

   static const char *sql =
       "INSERT INTO kb_doc_regions"
       " (chunk_id, document_key, page_no, x0, y0, x1, y1, quote, line_index, content_type,"
       "  sensitivity_class)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)"
       " RETURNING id";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", chunk_id);
   aimee_pg_bind_text(st, "?2", document_key ? document_key : "");
   aimee_pg_bind_int(st, "?3", page_no);
   aimee_pg_bind_double(st, "?4", x0);
   aimee_pg_bind_double(st, "?5", y0);
   aimee_pg_bind_double(st, "?6", x1);
   aimee_pg_bind_double(st, "?7", y1);
   aimee_pg_bind_text(st, "?8", quote ? quote : "");
   aimee_pg_bind_int(st, "?9", line_index);
   aimee_pg_bind_text(st, "?10", content_type ? content_type : "text");
   aimee_pg_bind_text(st, "?11", sensitivity_class ? sensitivity_class : "");
   int64_t new_id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      new_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return new_id;
}

/* Build a case-folded LIKE pattern "%<escaped query>%" into `dst` (size `cap`). LIKE
 * metacharacters %, _, and the escape char \ are backslash-escaped so the query matches as
 * a literal substring. The query is lowercased to pair with lower(content) for a
 * case-insensitive match on both Postgres and the sqlite shim. */
static void build_like_pattern(const char *query, char *dst, size_t cap)
{
   size_t o = 0;
   if (cap == 0)
      return;
   if (o < cap - 1)
      dst[o++] = '%';
   for (const char *p = query; p && *p && o + 2 < cap; p++)
   {
      char c = *p;
      if (c == '%' || c == '_' || c == '\\')
         dst[o++] = '\\';
      dst[o++] = (char)tolower((unsigned char)c);
   }
   if (o < cap - 1)
      dst[o++] = '%';
   dst[o] = '\0';
}

/* Phase A3 answerability combiner — documented, fixed default weights (pinned by a
 * reference test). The score is a deterministic function of three query-scoped inputs
 * (top relevance, query-term coverage, hit saturation) plus a corpus-scoped table-fact
 * count (§B; weighted 0 until that phase lands). */
#define KBP_ANS_W_TOP        0.5
#define KBP_ANS_W_COVERAGE   0.3
#define KBP_ANS_W_SATURATION 0.2
#define KBP_ANS_TARGET_K     5
/* A full-substring lexical hit (the whole query appears in the chunk) is strong
 * evidence; vector hits use their cosine score directly. */
#define KBP_LEXICAL_MATCH_SCORE 0.8
/* Upper bound on PDF-vector candidates fetched per search (stack-buffer size). */
#define KBP_VECTOR_FANIN_MAX 64

static void kbp_answerability_label(double s, char *out, size_t n)
{
   const char *l = s < 0.15 ? "NONE" : s < 0.40 ? "LOW" : s < 0.66 ? "MEDIUM" : "HIGH";
   snprintf(out, n, "%s", l);
}

/* Case-insensitive substring test (portable; strcasestr is non-portable / not on the
 * Windows build). Returns 1 if `needle` occurs in `hay` ignoring ASCII case. */
static int kbp_ci_contains(const char *hay, const char *needle)
{
   if (!hay || !needle || !needle[0])
      return 0;
   size_t nl = strlen(needle);
   for (const char *p = hay; *p; p++)
   {
      size_t i = 0;
      while (i < nl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == nl)
         return 1;
   }
   return 0;
}

/* Query-term coverage: fraction of distinct whitespace-delimited query tokens that
 * appear (case-insensitively) in the concatenation of matched chunk contents. A pure
 * function of (query, matched rows), so the same (query, corpus state) always yields the
 * same value. */
static double kbp_query_coverage(const char *query, const db2_kb_pdf_chunk_t *rows, int n)
{
   if (!query || n <= 0)
      return 0.0;
   char seen[32][128]; /* DISTINCT tokens — coverage is a fraction of distinct query terms */
   int n_seen = 0;
   int total = 0, covered = 0;
   char tok[128];
   const char *p = query;
   while (*p)
   {
      while (*p && isspace((unsigned char)*p))
         p++;
      int t = 0;
      while (*p && !isspace((unsigned char)*p))
      {
         if (t < (int)sizeof(tok) - 1)
            tok[t++] = *p;
         p++; /* always advance so a >127-char token is consumed whole, not re-split */
      }
      tok[t] = '\0';
      if (t == 0)
         continue;
      int dup = 0;
      for (int s = 0; s < n_seen; s++)
         if (strcmp(seen[s], tok) == 0)
         {
            dup = 1;
            break;
         }
      if (dup)
         continue;
      if (n_seen < (int)(sizeof(seen) / sizeof(seen[0])))
         snprintf(seen[n_seen++], sizeof(seen[0]), "%s", tok);
      total++;
      for (int i = 0; i < n; i++)
      {
         if (kbp_ci_contains(rows[i].content, tok))
         {
            covered++;
            break;
         }
      }
   }
   return total > 0 ? (double)covered / (double)total : 0.0;
}

/* Compute the Phase A3 answerability judgment from the merged candidate set. */
static void kbp_compute_answerability(const char *query, const db2_kb_pdf_chunk_t *rows, int n,
                                      db2_kb_answerability_t *ans)
{
   memset(ans, 0, sizeof(*ans));
   double top = 0.0;
   for (int i = 0; i < n; i++)
      if (rows[i].score > top)
         top = rows[i].score;
   double coverage = kbp_query_coverage(query, rows, n);
   double saturation = (double)n / (double)KBP_ANS_TARGET_K;
   if (saturation > 1.0)
      saturation = 1.0;
   double score =
       KBP_ANS_W_TOP * top + KBP_ANS_W_COVERAGE * coverage + KBP_ANS_W_SATURATION * saturation;
   if (score < 0.0)
      score = 0.0;
   if (score > 1.0)
      score = 1.0;
   ans->score = score;
   ans->top_score = top;
   ans->coverage = coverage;
   ans->saturation = saturation;
   ans->table_facts = 0; /* §B: folds in table-cell facts for query entities once built. */
   kbp_answerability_label(score, ans->label, sizeof(ans->label));
}

/* Find the index of chunk_id in out[0..n), or -1. */
static int kbp_find_chunk(const db2_kb_pdf_chunk_t *out, int n, int64_t chunk_id)
{
   for (int i = 0; i < n; i++)
      if (out[i].chunk_id == chunk_id)
         return i;
   return -1;
}

/* Fetch a single PDF chunk row by id, applying the SAME withhold + scope filters as the
 * lexical leg (doc_kind='pdf' AND quarantine_state<>'pending' AND, when scoped, project).
 * Re-checking project here means the access scope rides the authoritative kb_documents row,
 * not the denormalized kb_pdf_embeddings.project — so a stale/mismatched vector project
 * cannot surface a chunk the lexical leg would not. Returns 1 if written, else 0. */
static int kbp_fetch_pdf_chunk(void *conn, const char *project, int64_t id, db2_kb_pdf_chunk_t *row)
{
   int has_project = (project && project[0]);
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       has_project
           ? "SELECT id, file_path, content, page_start, page_end, sensitivity_class FROM "
             "kb_documents WHERE id = ?1 AND doc_kind = 'pdf' AND quarantine_state <> 'pending'"
             " AND project = ?2"
           : "SELECT id, file_path, content, page_start, page_end, sensitivity_class FROM "
             "kb_documents WHERE id = ?1 AND doc_kind = 'pdf' AND quarantine_state <> 'pending'",
       err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", id);
   if (has_project)
      aimee_pg_bind_text(st, "?2", project);
   int got = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(row, 0, sizeof(*row));
      row->chunk_id = aimee_pg_column_int64(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *ct = aimee_pg_column_text(st, 2);
      const char *sc = aimee_pg_column_text(st, 5);
      snprintf(row->document_key, sizeof(row->document_key), "%s", fp ? fp : "");
      snprintf(row->content, sizeof(row->content), "%s", ct ? ct : "");
      row->page_start = aimee_pg_column_int(st, 3);
      row->page_end = aimee_pg_column_int(st, 4);
      snprintf(row->sensitivity_class, sizeof(row->sensitivity_class), "%s", sc ? sc : "");
      got = 1;
   }
   aimee_pg_finalize(st);
   return got;
}

int db2_kb_pdf_search_chunks(const char *project, const char *query, int max,
                             db2_kb_pdf_chunk_t *out, db2_kb_answerability_t *ans_out)
{
   if (ans_out)
      memset(ans_out, 0, sizeof(*ans_out));
   if (!out || max <= 0 || !query)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   int has_project = (project && project[0]);

   /* ---- Stage 1a: lexical candidates (the always-on, embedder-independent leg). ---- */
   const char *sql =
       has_project ? "SELECT id, file_path, content, page_start, page_end, sensitivity_class"
                     " FROM kb_documents"
                     " WHERE doc_kind = 'pdf' AND quarantine_state <> 'pending' AND project = ?1"
                     "   AND lower(content) LIKE ?2 ESCAPE '\\'"
                     " ORDER BY id LIMIT ?3"
                   : "SELECT id, file_path, content, page_start, page_end, sensitivity_class"
                     " FROM kb_documents"
                     " WHERE doc_kind = 'pdf' AND quarantine_state <> 'pending'"
                     "   AND lower(content) LIKE ?1 ESCAPE '\\'"
                     " ORDER BY id LIMIT ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;

   char pat[512];
   build_like_pattern(query, pat, sizeof(pat));
   if (has_project)
   {
      aimee_pg_bind_text(st, "?1", project);
      aimee_pg_bind_text(st, "?2", pat);
      aimee_pg_bind_int(st, "?3", max);
   }
   else
   {
      aimee_pg_bind_text(st, "?1", pat);
      aimee_pg_bind_int(st, "?2", max);
   }

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].chunk_id = aimee_pg_column_int64(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *ct = aimee_pg_column_text(st, 2);
      const char *sc = aimee_pg_column_text(st, 5);
      snprintf(out[n].document_key, sizeof(out[n].document_key), "%s", fp ? fp : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", ct ? ct : "");
      out[n].page_start = aimee_pg_column_int(st, 3);
      out[n].page_end = aimee_pg_column_int(st, 4);
      snprintf(out[n].sensitivity_class, sizeof(out[n].sensitivity_class), "%s", sc ? sc : "");
      out[n].score = KBP_LEXICAL_MATCH_SCORE;
      out[n].matched_vector = 0;
      n++;
   }
   aimee_pg_finalize(st);

   /* ---- Stage 1b: vector candidates over the ISOLATED kb_pdf_embeddings relation. ----
    * Only when the capability is on AND an embedder is configured; otherwise the search
    * degrades to lexical-only (the Phase-2 behaviour). The vector leg reads ONLY
    * kb_pdf_embeddings (pgvec_kbpdf_search), so it can never surface a general-corpus or a
    * withheld document; kbp_fetch_pdf_chunk re-applies the withhold filters as the
    * candidate→row resolution (defense in depth). Vector candidates whose chunk row is
    * gone are simply skipped — never dropped from a join that would also drop lexical
    * hits. */
   config_t cfg;
   if (n < max && config_load(&cfg) == 0 && cfg.kb_pdf_vector_enabled)
   {
      const char *embed_cmd = config_embedding_command(&cfg, NULL);
      if (embed_cmd && embed_cmd[0])
      {
         float qvec[EMBED_MAX_DIM];
         int dim = memory_embed_text(query, embed_cmd, qvec, EMBED_MAX_DIM);
         if (dim > 0)
         {
            /* Request enough candidates to fill the remaining result budget even after
             * dedup against the lexical hits — 2x headroom for overlap — bounded by a fixed
             * fan-in cap so the stack buffers stay small. (Sizing off `max` rather than a
             * hard 32 lets a large-max caller actually reach `max` vector hits.) */
            int64_t vids[KBP_VECTOR_FANIN_MAX];
            double vscores[KBP_VECTOR_FANIN_MAX];
            int want = max * 2;
            if (want > KBP_VECTOR_FANIN_MAX)
               want = KBP_VECTOR_FANIN_MAX;
            int vn = pgvec_kbpdf_search(has_project ? project : NULL, qvec, dim, want, vids,
                                        vscores, want);
            for (int i = 0; i < vn && n < max; i++)
            {
               int at = kbp_find_chunk(out, n, vids[i]);
               if (at >= 0)
               {
                  /* Already a lexical hit: keep the stronger relevance, flag vector. */
                  if (vscores[i] > out[at].score)
                     out[at].score = vscores[i];
                  out[at].matched_vector = 1;
                  continue;
               }
               if (kbp_fetch_pdf_chunk(conn, has_project ? project : NULL, vids[i], &out[n]))
               {
                  out[n].score = vscores[i];
                  out[n].matched_vector = 1;
                  n++;
               }
            }
         }
      }
   }

   if (ans_out)
      kbp_compute_answerability(query, out, n, ans_out);
   return n;
}

int db2_kb_async_count_kind(const char *kind)
{
   if (!kind || !*kind)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM kb_async_jobs WHERE kind = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", kind);
   int n = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_pdf_reembed_all(void)
{
   /* Re-enqueue an embed_pdf job for every retrievable (non-pending) PDF chunk.
    * Used by the dim-change reset, which truncates kb_pdf_embeddings: unlike
    * kb_embeddings (auto-backfilled by the doc-embed drain), PDF vectors are only
    * (re)derived from these jobs. No-op when the PDF-vector capability is off, so a
    * reset never leaves embed_pdf jobs draining with nowhere to land. */
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.kb_pdf_vector_enabled)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT id, project FROM kb_documents WHERE doc_kind = 'pdf' AND quarantine_state = ''", err,
       sizeof(err));
   if (!st)
      return 0;
   int n = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t id = aimee_pg_column_int64(st, 0);
      const char *proj = aimee_pg_column_text(st, 1);
      if (db2_kb_async_enqueue("embed_pdf", id, proj ? proj : "") == 0)
         n++;
      else
         LOG_WARN("kb_pdf", "reembed: embed_pdf enqueue failed for chunk %lld", (long long)id);
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_table_cell_insert(int64_t region_id, const char *document_key, int page_no, int cell_row,
                             int cell_col, const char *cell_text, const char *subject,
                             const char *relation, const char *object, int tsr_confidence,
                             const char *sensitivity_class)
{
   if (region_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "INSERT INTO kb_table_cells (region_id, document_key, page_no, cell_row, cell_col,"
       " cell_text, subject, relation, object, tsr_confidence, source_type, sensitivity_class)"
       " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, 'table_cell', ?11) RETURNING id";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", region_id);
   aimee_pg_bind_text(st, "?2", document_key ? document_key : "");
   aimee_pg_bind_int(st, "?3", page_no);
   aimee_pg_bind_int(st, "?4", cell_row);
   aimee_pg_bind_int(st, "?5", cell_col);
   aimee_pg_bind_text(st, "?6", cell_text ? cell_text : "");
   aimee_pg_bind_text(st, "?7", subject ? subject : "");
   aimee_pg_bind_text(st, "?8", relation ? relation : "");
   aimee_pg_bind_text(st, "?9", object ? object : "");
   aimee_pg_bind_int(st, "?10", tsr_confidence);
   aimee_pg_bind_text(st, "?11", sensitivity_class ? sensitivity_class : "");
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id > 0 ? (int)id : -1;
}

int db2_kb_table_cells_lookup(const char *project, const char *document_key, int page_no,
                              db2_kb_table_cell_t *out, int max)
{
   if (!out || max <= 0 || !project || !*project || !document_key || !*document_key)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Gate via a join to the AUTHORITATIVE kb_documents row: doc_kind='pdf' AND
    * quarantine_state<>'pending' AND project — so a guessed/foreign/withheld document_key
    * returns empty regardless of the denormalised columns on kb_table_cells. */
   int all_pages = (page_no < 0);
   const char *sql =
       all_pages
           ? "SELECT c.id, c.region_id, c.page_no, c.cell_row, c.cell_col, c.cell_text,"
             " c.subject, c.relation, c.object, c.tsr_confidence, c.sensitivity_class"
             " FROM kb_table_cells c JOIN kb_doc_regions r ON r.id = c.region_id"
             " JOIN kb_documents d ON d.id = r.chunk_id"
             " WHERE d.project = ?1 AND d.doc_kind = 'pdf' AND d.quarantine_state <> 'pending'"
             "   AND c.document_key = ?2"
             " ORDER BY c.page_no, c.cell_row, c.cell_col LIMIT ?3"
           : "SELECT c.id, c.region_id, c.page_no, c.cell_row, c.cell_col, c.cell_text,"
             " c.subject, c.relation, c.object, c.tsr_confidence, c.sensitivity_class"
             " FROM kb_table_cells c JOIN kb_doc_regions r ON r.id = c.region_id"
             " JOIN kb_documents d ON d.id = r.chunk_id"
             " WHERE d.project = ?1 AND d.doc_kind = 'pdf' AND d.quarantine_state <> 'pending'"
             "   AND c.document_key = ?2 AND c.page_no = ?3"
             " ORDER BY c.cell_row, c.cell_col LIMIT ?4";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", document_key);
   if (all_pages)
      aimee_pg_bind_int(st, "?3", max);
   else
   {
      aimee_pg_bind_int(st, "?3", page_no);
      aimee_pg_bind_int(st, "?4", max);
   }
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].id = aimee_pg_column_int64(st, 0);
      out[n].region_id = aimee_pg_column_int64(st, 1);
      out[n].page_no = aimee_pg_column_int(st, 2);
      out[n].cell_row = aimee_pg_column_int(st, 3);
      out[n].cell_col = aimee_pg_column_int(st, 4);
      const char *ctext = aimee_pg_column_text(st, 5);
      const char *subj = aimee_pg_column_text(st, 6);
      const char *rel = aimee_pg_column_text(st, 7);
      const char *obj = aimee_pg_column_text(st, 8);
      const char *sens = aimee_pg_column_text(st, 10);
      snprintf(out[n].cell_text, sizeof(out[n].cell_text), "%s", ctext ? ctext : "");
      snprintf(out[n].subject, sizeof(out[n].subject), "%s", subj ? subj : "");
      snprintf(out[n].relation, sizeof(out[n].relation), "%s", rel ? rel : "");
      snprintf(out[n].object, sizeof(out[n].object), "%s", obj ? obj : "");
      out[n].tsr_confidence = aimee_pg_column_int(st, 9);
      snprintf(out[n].sensitivity_class, sizeof(out[n].sensitivity_class), "%s", sens ? sens : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

void db2_kb_documents_set_tsr_state(const char *project, const char *file_path, const char *state)
{
   void *conn = db2_conn();
   if (!conn || !project || !*project || !file_path || !*file_path)
      return;
   static const char *sql = "UPDATE kb_documents SET tsr_state = ?3"
                            " WHERE project = ?1 AND file_path = ?2 AND doc_kind = 'pdf'";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", file_path);
   aimee_pg_bind_text(st, "?3", state ? state : "");
   (void)aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
}

int db2_kb_pdf_tsr_state(const char *project, const char *document_key, char *out, size_t out_len)
{
   if (out && out_len)
      out[0] = '\0';
   if (!out || !out_len || !project || !*project || !document_key || !*document_key)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Same ACL as lookup: only a readable (non-withheld) PDF doc yields a state. */
   static const char *sql = "SELECT tsr_state FROM kb_documents"
                            " WHERE project = ?1 AND file_path = ?2 AND doc_kind = 'pdf'"
                            "   AND quarantine_state <> 'pending' LIMIT 1";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", document_key);
   int hit = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *s = aimee_pg_column_text(st, 0);
      snprintf(out, out_len, "%s", s ? s : "");
      hit = 1;
   }
   aimee_pg_finalize(st);
   return hit;
}

int db2_kb_doc_regions_for_chunk(int64_t chunk_id, db2_kb_pdf_region_t *out, int max)
{
   if (!out || max <= 0 || chunk_id <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;

   static const char *sql = "SELECT page_no, x0, y0, x1, y1, quote, line_index, content_type"
                            " FROM kb_doc_regions WHERE chunk_id = ?1 ORDER BY line_index LIMIT ?2";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", chunk_id);
   aimee_pg_bind_int(st, "?2", max);

   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].page_no = aimee_pg_column_int(st, 0);
      out[n].x0 = aimee_pg_column_double(st, 1);
      out[n].y0 = aimee_pg_column_double(st, 2);
      out[n].x1 = aimee_pg_column_double(st, 3);
      out[n].y1 = aimee_pg_column_double(st, 4);
      const char *q = aimee_pg_column_text(st, 5);
      out[n].line_index = aimee_pg_column_int(st, 6);
      const char *ctype = aimee_pg_column_text(st, 7);
      snprintf(out[n].quote, sizeof(out[n].quote), "%s", q ? q : "");
      snprintf(out[n].content_type, sizeof(out[n].content_type), "%s", ctype ? ctype : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

/* Run a quarantine-admin statement that ends in `RETURNING id`, counting the rows it
 * actually affected. Using RETURNING makes the transition atomic — there is no count-then-act
 * race, and the scoped predicate (carried in `sql`) is the ONLY thing acted on, so the count
 * reported is exactly what changed. Returns the affected-row count (>=0), or -1 on error. */
static int kb_pdf_quarantine_apply(const char *sql, const char *project, const char *document_key)
{
   void *conn = db2_conn();
   if (!conn || !project || !*project || !document_key || !*document_key)
      return -1;
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", document_key);
   int n = 0, rc;
   while ((rc = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
      n++;
   aimee_pg_finalize(st);
   return rc == AIMEE_PG_DONE ? n : -1;
}

int db2_kb_pdf_quarantine_confirm(const char *project, const char *document_key)
{
   /* Scoped to exactly the pending PDF chunks at this (project, file_path); RETURNING gives
    * the true affected count. A confirmed doc has quarantine_state='' and is retrievable. */
   static const char *sql = "UPDATE kb_documents SET quarantine_state = ''"
                            " WHERE project = ?1 AND file_path = ?2 AND doc_kind = 'pdf'"
                            "   AND quarantine_state = 'pending'"
                            " RETURNING id";
   int n = kb_pdf_quarantine_apply(sql, project, document_key);
   if (n <= 0)
      return n;

   /* Phase A1: a confirmed (formerly restricted) doc must become vector-retrievable.
    * Re-select its now-unpending chunk ids (a document_key carries one sensitivity
    * class, so quarantine_state='' after confirm is exactly the just-confirmed
    * chunks) and enqueue an idempotent embed_pdf job per chunk when the capability
    * is on. Row-by-row so an arbitrarily large doc is fully covered. */
   config_t cfg;
   if (config_load(&cfg) != 0 || !cfg.kb_pdf_vector_enabled)
      return n;
   void *conn = db2_conn();
   if (!conn)
      return n;
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn,
                        "SELECT id FROM kb_documents WHERE project = ?1 AND file_path = ?2"
                        "   AND doc_kind = 'pdf' AND quarantine_state = ''",
                        err, sizeof(err));
   if (!st)
      return n;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", document_key);
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      int64_t id = aimee_pg_column_int64(st, 0);
      /* Best-effort: a failed enqueue leaves the confirmed chunk lexical-only (still fully
       * retrievable + cited), recoverable by the dim-reset reembed or a re-confirm. Log it
       * so a silent vector gap is observable rather than invisible. */
      if (db2_kb_async_enqueue("embed_pdf", id, project) != 0)
         LOG_WARN("kb_pdf", "confirm: embed_pdf enqueue failed for chunk %lld (%s)", (long long)id,
                  document_key);
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_pdf_quarantine_reject(const char *project, const char *document_key)
{
   /* Delete ONLY the pending PDF chunks at this (project, file_path) — NOT every row sharing
    * the file_path (a non-PDF or already-confirmed doc could collide). Regions cascade via
    * the kb_doc_regions FK. RETURNING gives the true deleted count. */
   static const char *sql = "DELETE FROM kb_documents"
                            " WHERE project = ?1 AND file_path = ?2 AND doc_kind = 'pdf'"
                            "   AND quarantine_state = 'pending'"
                            " RETURNING id";
   return kb_pdf_quarantine_apply(sql, project, document_key);
}

static void fill_region_row(aimee_pg_stmt_t *st, db2_kb_pdf_region_t *r)
{
   memset(r, 0, sizeof(*r));
   r->page_no = aimee_pg_column_int(st, 0);
   r->x0 = aimee_pg_column_double(st, 1);
   r->y0 = aimee_pg_column_double(st, 2);
   r->x1 = aimee_pg_column_double(st, 3);
   r->y1 = aimee_pg_column_double(st, 4);
   const char *q = aimee_pg_column_text(st, 5);
   r->line_index = aimee_pg_column_int(st, 6);
   const char *ct = aimee_pg_column_text(st, 7);
   snprintf(r->quote, sizeof(r->quote), "%s", q ? q : "");
   snprintf(r->content_type, sizeof(r->content_type), "%s", ct ? ct : "");
}

int db2_kb_pdf_open_page(const char *project, const char *document_key, int page_no,
                         db2_kb_pdf_region_t *out, int max)
{
   if (!out || max <= 0 || !project || !*project || !document_key || !*document_key)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* Join to kb_documents so the chunk's quarantine gate applies — a pending (restricted)
    * document's page is withheld even though kb_doc_regions has no quarantine column. */
   static const char *sql =
       "SELECT r.page_no, r.x0, r.y0, r.x1, r.y1, r.quote, r.line_index, r.content_type"
       " FROM kb_doc_regions r JOIN kb_documents d ON d.id = r.chunk_id"
       " WHERE r.document_key = ?1 AND r.page_no = ?2 AND d.project = ?3"
       "   AND d.doc_kind = 'pdf' AND d.quarantine_state <> 'pending'"
       " ORDER BY r.line_index LIMIT ?4";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", document_key);
   aimee_pg_bind_int(st, "?2", page_no);
   aimee_pg_bind_text(st, "?3", project);
   aimee_pg_bind_int(st, "?4", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      fill_region_row(st, &out[n++]);
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_pdf_open_neighbors(const char *project, int64_t chunk_id, db2_kb_pdf_chunk_t *out,
                              int max)
{
   if (!out || max <= 0 || chunk_id <= 0 || !project || !*project)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   /* The prev/next chunks of chunk_id in reading order (via prev_chunk_id/next_chunk_id), each
    * subject to the quarantine gate AND scoped to `project` — without the project predicate a
    * caller could enumerate chunk_ids and read another scope's PDF content (cross-scope IDOR),
    * so both the anchor chunk and its neighbours must be in the caller's project. */
   static const char *sql =
       "SELECT n.id, n.file_path, n.content, n.page_start, n.page_end, n.sensitivity_class"
       " FROM kb_documents c JOIN kb_documents n"
       "   ON (n.id = c.prev_chunk_id OR n.id = c.next_chunk_id)"
       " WHERE c.id = ?1 AND c.project = ?2 AND n.project = ?2"
       "   AND c.quarantine_state <> 'pending'" /* don't navigate FROM a quarantined chunk */
       "   AND n.doc_kind = 'pdf' AND n.quarantine_state <> 'pending'"
       " ORDER BY n.chunk_index LIMIT ?3";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", chunk_id);
   aimee_pg_bind_text(st, "?2", project);
   aimee_pg_bind_int(st, "?3", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].chunk_id = aimee_pg_column_int64(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *ct = aimee_pg_column_text(st, 2);
      const char *sc = aimee_pg_column_text(st, 5);
      snprintf(out[n].document_key, sizeof(out[n].document_key), "%s", fp ? fp : "");
      snprintf(out[n].content, sizeof(out[n].content), "%s", ct ? ct : "");
      out[n].page_start = aimee_pg_column_int(st, 3);
      out[n].page_end = aimee_pg_column_int(st, 4);
      snprintf(out[n].sensitivity_class, sizeof(out[n].sensitivity_class), "%s", sc ? sc : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_kb_pdf_inspect_structure(const char *project, const char *document_key,
                                 db2_kb_pdf_outline_t *out, int max)
{
   if (!out || max <= 0 || !project || !*project || !document_key || !*document_key)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql =
       "SELECT chunk_index, page_start, page_end, heading_path FROM kb_documents"
       " WHERE project = ?1 AND file_path = ?2 AND doc_kind = 'pdf'"
       "   AND quarantine_state <> 'pending'"
       " ORDER BY chunk_index LIMIT ?3";
   char err[KBP_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", document_key);
   aimee_pg_bind_int(st, "?3", max);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      memset(&out[n], 0, sizeof(out[n]));
      out[n].chunk_index = aimee_pg_column_int(st, 0);
      out[n].page_start = aimee_pg_column_int(st, 1);
      out[n].page_end = aimee_pg_column_int(st, 2);
      const char *hp = aimee_pg_column_text(st, 3);
      snprintf(out[n].heading_path, sizeof(out[n].heading_path), "%s", hp ? hp : "");
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}
