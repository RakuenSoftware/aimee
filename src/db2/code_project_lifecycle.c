/* code_project_lifecycle.c: recoverable identity lifecycle for the code index. */
#include "code_project_lifecycle.h"

#include "aimee.h"
#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"
#include "kb_audit_worm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <openssl/evp.h>

#define CPL_ERRBUF              256
#define CPL_JSON_ESCAPED_CAP(n) ((n) * 6 + 1)
#define CPL_AUDIT_DETAIL_CAP    16384

typedef struct
{
   const char *name;
   const char *where_sql;
   int optional;
} cpl_target_spec_t;

static int cpl_begin(void *conn, char *err, size_t err_len)
{
   /* Confirmation must not delete rows that arrived after its manifest count.
    * PostgreSQL SERIALIZABLE predicate-locks the counted target sets, making a
    * concurrent ingest abort one side instead of silently broadening the purge.
    * The SQLite test shim has one writer and accepts only plain BEGIN. */
   return aimee_pg_exec(conn, aimee_pg_is_shim() ? "BEGIN" : "BEGIN ISOLATION LEVEL SERIALIZABLE",
                        err, err_len);
}

static int cpl_digest_init(EVP_MD_CTX **ctx)
{
   *ctx = EVP_MD_CTX_new();
   if (*ctx && EVP_DigestInit_ex(*ctx, EVP_sha256(), NULL) == 1)
      return 0;
   EVP_MD_CTX_free(*ctx);
   *ctx = NULL;
   return -1;
}

static int cpl_digest_data(EVP_MD_CTX *ctx, const void *value, size_t len)
{
   return len == 0 || (value && EVP_DigestUpdate(ctx, value, len) == 1) ? 0 : -1;
}

static int cpl_digest_text(EVP_MD_CTX *ctx, const char *value)
{
   return cpl_digest_data(ctx, value ? value : "", strlen(value ? value : ""));
}

static int cpl_digest_finish(EVP_MD_CTX *ctx, char *out, size_t out_cap)
{
   unsigned char digest[EVP_MAX_MD_SIZE];
   unsigned int len = 0;
   int ok = ctx && EVP_DigestFinal_ex(ctx, digest, &len) == 1 && len == 32 && out_cap >= 72;
   EVP_MD_CTX_free(ctx);
   if (!ok)
      return -1;
   int pos = snprintf(out, out_cap, "sha256:");
   for (unsigned int i = 0; i < len; i++)
      pos += snprintf(out + pos, out_cap - (size_t)pos, "%02x", digest[i]);
   return 0;
}

/* Ordered for deletion: references first, owning project last. Durable memory
 * and lessons ledgers are intentionally not index-purge targets. */
static const cpl_target_spec_t PURGE_TARGETS[] = {
    {"file_exports",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"file_imports",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"terms",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"code_calls",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"file_contents",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"css_component_styles",
     "component_file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE "
     "p.name=?1)",
     0},
    {"css_rules",
     "file_id IN (SELECT f.id FROM files f JOIN projects p ON p.id=f.project_id WHERE p.name=?1)",
     0},
    {"entity_edges",
     "source IN (SELECT node_key FROM entity_nodes WHERE project=?1) OR target IN (SELECT node_key "
     "FROM entity_nodes WHERE project=?1)",
     0},
    {"entity_node_aliases", "project=?1", 0},
    {"entity_nodes", "project=?1", 0},
    {"code_projection_edges", "project=?1", 1},
    {"code_projection_communities", "project=?1", 1},
    {"code_projection_generations", "project=?1", 1},
    {"cross_repo_review_queue", "caller_repo=?1 OR candidate_definer=?1", 0},
    {"cross_repo_route", "caller_project=?1 OR definer_project=?1", 0},
    {"cross_repo_build_dep", "caller_project=?1 OR definer_project=?1", 0},
    {"cross_repo_identity", "project=?1", 0},
    {"code_embeddings", "project=?1", 1},
    {"code_index_ops", "project=?1", 0},
    {"kb_code_unit_jobs", "project=?1", 0},
    {"kb_embeddings", "project=?1", 1},
    {"kb_pdf_embeddings", "project=?1", 1},
    {"curator_code_unit_vectors",
     "artifact_id IN (SELECT id FROM artifacts WHERE scope_kind='project' AND scope_id=?1)", 1},
    {"kb_async_jobs", "project=?1", 0},
    {"kb_ingest_queue", "project=?1", 0},
    {"kb_file_index", "project=?1", 0},
    {"vector_index_ops",
     "memory_id IS NULL AND collection IN ('kb_chunks','kb_embeddings','kb_pdf_embeddings')"
     " AND point_id IN (SELECT id FROM kb_documents WHERE project=?1)",
     0},
    {"kb_doc_assets", "project=?1", 0},
    {"kb_documents", "project=?1", 0},
    {"kb_lsh_buckets", "project=?1", 0},
    {"kb_minhash_signatures", "project=?1", 0},
    {"css_render_snapshots", "project=?1", 0},
    {"css_migration_units", "project=?1", 0},
    {"kb_runtime_state",
     "state_key IN ('code_scan_sha:'||?1, 'code_embed_sig:'||?1, 'cochange_head:'||?1,"
     " 'surprising_judged:'||?1, 'surprising_confirmed:'||?1, 'project_purging:'||?1,"
     " 'project_purging_ts:'||?1)",
     0},
    {"code_project_aliases", "project_id IN (SELECT id FROM projects WHERE name=?1)", 0},
    {"code_project_generations", "project_id IN (SELECT id FROM projects WHERE name=?1)", 0},
    {"files", "project_id IN (SELECT id FROM projects WHERE name=?1)", 0},
    {"projects", "name=?1", 0},
};

#define CPL_RETIRED_GENERATIONS                                                                    \
   "SELECT generation FROM code_project_generations WHERE project_id IN"                           \
   " (SELECT id FROM projects WHERE name=?1) AND state<>'current' AND detached_at<>''"             \
   " AND detached_at<?2"

/* Children precede owners. Operational/vector rows derived from a retained
 * source generation retire with that generation so neither storage nor an
 * out-of-band maintenance read can resurrect stale content after re-attach. */
static const cpl_target_spec_t GC_TARGETS[] = {
    {"code_project_aliases",
     "project_id IN (SELECT id FROM projects WHERE name=?1) AND is_current=0 AND last_seen_at<?2",
     0},
    {"code_embeddings", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 1},
    {"vector_index_ops",
     "memory_id IS NULL AND point_id IN (SELECT id FROM kb_documents WHERE project=?1 AND "
     "generation IN (" CPL_RETIRED_GENERATIONS "))",
     0},
    {"kb_async_jobs",
     "document_id IN (SELECT id FROM kb_documents WHERE project=?1 AND generation IN "
     "(" CPL_RETIRED_GENERATIONS "))",
     0},
    {"kb_code_unit_jobs", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"kb_embeddings",
     "point_id IN (SELECT id FROM kb_documents WHERE project=?1 AND generation IN "
     "(" CPL_RETIRED_GENERATIONS "))",
     1},
    {"kb_pdf_embeddings",
     "point_id IN (SELECT id FROM kb_documents WHERE project=?1 AND generation IN "
     "(" CPL_RETIRED_GENERATIONS "))",
     1},
    {"kb_doc_assets", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"kb_documents", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"kb_file_index", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"kb_lsh_buckets", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"kb_minhash_signatures", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"css_render_snapshots", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"css_migration_units", "project=?1 AND generation IN (" CPL_RETIRED_GENERATIONS ")", 0},
    {"files",
     "project_id IN (SELECT id FROM projects WHERE name=?1) AND generation IN "
     "(" CPL_RETIRED_GENERATIONS ")",
     0},
    {"code_project_generations",
     "project_id IN (SELECT id FROM projects WHERE name=?1) AND state<>'current' AND "
     "detached_at<>'' AND detached_at<?2",
     0},
};

static int cpl_project_generation(void *conn, const char *project, int64_t *generation)
{
   char err[CPL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT current_generation FROM projects WHERE name=?1", err, sizeof(err));
   if (!st)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   aimee_pg_bind_text(st, "?1", project);
   int rc = CODE_PROJECT_LIFECYCLE_NOT_FOUND;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      *generation = aimee_pg_column_int64(st, 0);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

static int cpl_optional_table_exists(void *conn, const cpl_target_spec_t *spec)
{
   if (!spec->optional)
      return 1;
   char err[CPL_ERRBUF] = "";
   const char *sql = aimee_pg_is_shim()
                         ? "SELECT name FROM sqlite_master WHERE type='table' AND name=?1"
                         : "SELECT to_regclass(?1)";
   aimee_pg_stmt_t *probe = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!probe)
      return -1;
   aimee_pg_bind_text(probe, "?1", spec->name);
   int exists = 0;
   if (aimee_pg_step(probe, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *resolved = aimee_pg_column_text(probe, 0);
      exists = resolved && resolved[0];
   }
   aimee_pg_finalize(probe);
   return exists;
}

/* Measure and fingerprint the exact physical rows in one ordered read.  On
 * PostgreSQL, tableoid/ctid/xmin distinguish a replacement row even if its
 * values and the table's count are unchanged.  Updates and rewrites may cause
 * conservative confirmation failures, which is preferable to deleting a row
 * that the operator never reviewed.  The SQLite test shim fingerprints rowid
 * plus every value so SQLite's legal rowid reuse cannot mask a replacement. */
static int cpl_measure(void *conn, const cpl_target_spec_t *spec, const char *project,
                       const char *arg2, code_project_target_t *out)
{
   memset(out, 0, sizeof(*out));
   snprintf(out->table, sizeof(out->table), "%s", spec->name);
   EVP_MD_CTX *digest = NULL;
   if (cpl_digest_init(&digest) != 0)
      return -1;
   int exists = cpl_optional_table_exists(conn, spec);
   if (exists < 0)
   {
      EVP_MD_CTX_free(digest);
      return -1;
   }
   if (!exists)
      return cpl_digest_finish(digest, out->fingerprint, sizeof(out->fingerprint));
   char sql[1024];
   snprintf(sql, sizeof(sql),
            aimee_pg_is_shim()
                ? "SELECT rowid,* FROM %s WHERE %s ORDER BY rowid"
                : "SELECT tableoid::text||':'||ctid::text||':'||xmin::text FROM %s WHERE %s "
                  "ORDER BY 1",
            spec->name, spec->where_sql);
   char err[CPL_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      EVP_MD_CTX_free(digest);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", project);
   if (arg2)
      aimee_pg_bind_text(st, "?2", arg2);
   int step;
   while ((step = aimee_pg_step(st, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      if (aimee_pg_is_shim())
      {
         /* These typed accessors are part of the shared adapter contract:
          * db_postgres.c implements both native backends and the unit-test
          * SQLite shim implements the same API.  Hash raw bytes so embedded
          * NULs and type differences cannot collapse distinct target rows. */
         int columns = aimee_pg_column_count(st);
         for (int col = 0; col < columns; col++)
         {
            char marker[64];
            int type = aimee_pg_column_type(st, col);
            int bytes = aimee_pg_column_bytes(st, col);
            snprintf(marker, sizeof(marker), "\n%d:%d:", type, bytes);
            if (cpl_digest_text(digest, marker) != 0)
               step = AIMEE_PG_ERR;
            if (!aimee_pg_column_is_null(st, col))
               if (cpl_digest_data(digest, aimee_pg_column_blob(st, col), (size_t)bytes) != 0)
                  step = AIMEE_PG_ERR;
         }
      }
      else
      {
         if (cpl_digest_text(digest, aimee_pg_column_text(st, 0)) != 0 ||
             cpl_digest_text(digest, "\n") != 0)
            step = AIMEE_PG_ERR;
      }
      if (step == AIMEE_PG_ERR)
         break;
      out->rows++;
   }
   aimee_pg_finalize(st);
   if (step != AIMEE_PG_DONE)
   {
      EVP_MD_CTX_free(digest);
      return -1;
   }
   return cpl_digest_finish(digest, out->fingerprint, sizeof(out->fingerprint));
}

static int cpl_hash_manifest(code_project_manifest_t *out)
{
   EVP_MD_CTX *digest = NULL;
   if (cpl_digest_init(&digest) != 0)
      return -1;
   char line[256];
   if (cpl_digest_text(digest, out->operation) != 0 || cpl_digest_text(digest, "\n") != 0 ||
       cpl_digest_text(digest, out->project) != 0)
      goto fail;
   /* Mode is response/audit state, not target identity.  Excluding it keeps the
    * dry-run authorization hash identical to the confirmed response while the
    * selected rows, operation, generation, and criteria remain unchanged. */
   snprintf(line, sizeof(line), "\n%lld\n%s\n", (long long)out->generation, out->criteria);
   if (cpl_digest_text(digest, line) != 0)
      goto fail;
   for (int i = 0; i < out->target_count; i++)
   {
      snprintf(line, sizeof(line), "%s=%ld:%s\n", out->targets[i].table, out->targets[i].rows,
               out->targets[i].fingerprint);
      if (cpl_digest_text(digest, line) != 0)
         goto fail;
   }
   return cpl_digest_finish(digest, out->manifest_hash, sizeof(out->manifest_hash));
fail:
   EVP_MD_CTX_free(digest);
   return -1;
}

static int cpl_manifest(void *conn, const char *operation, const char *project, const char *mode,
                        const cpl_target_spec_t *specs, int spec_count,
                        code_project_manifest_t *out)
{
   if (!conn || !operation || !operation[0] || !project || !project[0] || !out ||
       strlen(operation) >= sizeof(out->operation) || strlen(project) >= sizeof(out->project) ||
       spec_count > CODE_PROJECT_MANIFEST_MAX_TARGETS)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   memset(out, 0, sizeof(*out));
   snprintf(out->operation, sizeof(out->operation), "%s", operation);
   snprintf(out->project, sizeof(out->project), "%s", project);
   snprintf(out->mode, sizeof(out->mode), "%s", mode);
   int rc = cpl_project_generation(conn, project, &out->generation);
   if (rc != 0)
      return rc;
   for (int i = 0; i < spec_count; i++)
   {
      if (cpl_measure(conn, &specs[i], project, NULL, &out->targets[i]) != 0)
         return CODE_PROJECT_LIFECYCLE_ERROR;
      out->total_rows += out->targets[i].rows;
   }
   out->target_count = spec_count;
   return cpl_hash_manifest(out) == 0 ? 0 : CODE_PROJECT_LIFECYCLE_ERROR;
}

static void cpl_json_escape(char *out, size_t cap, const char *in);

int db2_code_project_detach(const char *project, const char *principal, int64_t *generation_out)
{
   if (!project || !project[0] || !principal || !principal[0] || strlen(principal) > 575 ||
       strlen(project) >= sizeof(((code_project_manifest_t *)0)->project))
      return CODE_PROJECT_LIFECYCLE_ERROR;
   void *conn = db2_conn();
   if (!conn)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   char err[CPL_ERRBUF] = "";
   if (cpl_begin(conn, err, sizeof(err)) != 0)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   int64_t generation = 0;
   int rc = cpl_project_generation(conn, project, &generation);
   if (rc != 0)
      goto rollback;
   char ts[32];
   now_utc(ts, sizeof(ts));
   char actor[CPL_JSON_ESCAPED_CAP(575)], subject[CPL_JSON_ESCAPED_CAP(255)], detail[8192];
   cpl_json_escape(actor, sizeof(actor), principal);
   cpl_json_escape(subject, sizeof(subject), project);
   snprintf(detail, sizeof(detail),
            "{\"principal\":\"%s\",\"operation\":\"detach\","
            "\"stable_project_id\":\"%s\",\"generation\":%lld,"
            "\"timestamp\":\"%s\",\"state\":\"detached\"}",
            actor, subject, (long long)generation, ts);
   if (db2_kb_audit_append_in_txn(conn, "operator", principal, "code.index.detach", project,
                                  "allow", detail) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_AUDIT_FAILED;
      goto rollback;
   }
   const char *updates[] = {
       /* entity_edges is the published projection cache; the generation ledger
        * remains below for audit/recovery, but detached code must stop leaking
        * through generic graph readers immediately. */
       "DELETE FROM entity_edges WHERE edge_origin='code_projection'"
       " AND projection_generation_id IN (SELECT id FROM code_projection_generations"
       " WHERE project=?1)",
       "UPDATE projects SET lifecycle_state='detached' WHERE name=?1",
       "UPDATE code_project_generations SET state='detached', detached_at=?2"
       " WHERE project_id IN (SELECT id FROM projects WHERE name=?1) AND generation=?3",
       "UPDATE code_project_aliases SET is_current=0"
       " WHERE project_id IN (SELECT id FROM projects WHERE name=?1)",
       "UPDATE code_projection_generations SET state=CASE"
       " WHEN state='visible' THEN 'superseded' ELSE 'aborted' END"
       " WHERE project=?1 AND state IN ('visible','pending')",
   };
   for (int i = 0; i < 5; i++)
   {
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, updates[i], err, sizeof(err));
      if (!st)
         goto rollback;
      aimee_pg_bind_text(st, "?1", project);
      if (i == 2)
      {
         aimee_pg_bind_text(st, "?2", ts);
         aimee_pg_bind_int64(st, "?3", generation);
      }
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto rollback;
      }
      aimee_pg_finalize(st);
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      goto rollback;
   if (generation_out)
      *generation_out = generation;
   return 0;
rollback:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc == CODE_PROJECT_LIFECYCLE_NOT_FOUND || rc == CODE_PROJECT_LIFECYCLE_AUDIT_FAILED
              ? rc
              : CODE_PROJECT_LIFECYCLE_ERROR;
}

int db2_code_project_purge_manifest(const char *project, code_project_manifest_t *out)
{
   void *conn = db2_conn();
   if (!conn)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   char err[CPL_ERRBUF] = "";
   if (cpl_begin(conn, err, sizeof(err)) != 0)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   int rc = cpl_manifest(conn, "purge", project, "dry_run", PURGE_TARGETS,
                         (int)(sizeof(PURGE_TARGETS) / sizeof(PURGE_TARGETS[0])), out);
   if (rc == 0 && aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0)
      return 0;
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc == CODE_PROJECT_LIFECYCLE_NOT_FOUND ? rc : CODE_PROJECT_LIFECYCLE_ERROR;
}

static void cpl_json_escape(char *out, size_t cap, const char *in)
{
   size_t p = 0;
   for (size_t i = 0; in && in[i]; i++)
   {
      unsigned char c = (unsigned char)in[i];
      if (c < 0x20)
      {
         if (p + 6 >= cap)
            break;
         int wrote = snprintf(out + p, cap - p, "\\u%04x", (unsigned int)c);
         if (wrote != 6)
            break;
         p += 6;
         continue;
      }
      if (p + (c == '"' || c == '\\' ? 2U : 1U) >= cap)
         break;
      if (c == '"' || c == '\\')
         out[p++] = '\\';
      out[p++] = (char)c;
   }
   out[p] = '\0';
}

static void cpl_audit_detail(const code_project_manifest_t *m, const char *principal,
                             const char *reason, char *out, size_t cap)
{
   char actor[CPL_JSON_ESCAPED_CAP(575)], project[CPL_JSON_ESCAPED_CAP(255)],
       why[CPL_JSON_ESCAPED_CAP(512)], criteria[CPL_JSON_ESCAPED_CAP(127)], ts[32];
   cpl_json_escape(actor, sizeof(actor), principal);
   cpl_json_escape(project, sizeof(project), m->project);
   cpl_json_escape(why, sizeof(why), reason);
   cpl_json_escape(criteria, sizeof(criteria), m->criteria);
   now_utc(ts, sizeof(ts));
   int pos = snprintf(out, cap,
                      "{\"principal\":\"%s\",\"operation\":\"%s\","
                      "\"stable_project_id\":\"%s\","
                      "\"generation\":%lld,\"timestamp\":\"%s\",\"reason\":\"%s\","
                      "\"mode\":\"confirmed\",\"criteria\":\"%s\","
                      "\"manifest_hash\":\"%s\",\"counts\":{",
                      actor, m->operation, project, (long long)m->generation, ts, why, criteria,
                      m->manifest_hash);
   for (int i = 0; i < m->target_count && pos > 0 && (size_t)pos < cap; i++)
      pos += snprintf(out + pos, cap - (size_t)pos, "%s\"%s\":%ld", i ? "," : "",
                      m->targets[i].table, m->targets[i].rows);
   if (pos > 0 && (size_t)pos < cap)
      snprintf(out + pos, cap - (size_t)pos, "},\"total\":%ld}", m->total_rows);
}

static int cpl_delete_targets(void *conn, const char *project, const cpl_target_spec_t *specs,
                              int spec_count)
{
   char err[CPL_ERRBUF] = "";
   for (int i = 0; i < spec_count; i++)
   {
      if (specs[i].optional)
      {
         int exists = cpl_optional_table_exists(conn, &specs[i]);
         if (exists < 0)
            return -1;
         if (!exists)
            continue;
      }
      char sql[1024];
      snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s", specs[i].name, specs[i].where_sql);
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         return -1;
      aimee_pg_bind_text(st, "?1", project);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         return -1;
      }
      aimee_pg_finalize(st);
   }
   return 0;
}

int db2_code_project_purge_confirm(const char *project, const char *expected_hash,
                                   const char *principal, const char *reason,
                                   code_project_manifest_t *out)
{
   if (!expected_hash || !expected_hash[0] || !principal || !principal[0] || !reason ||
       !reason[0] || strlen(principal) > 575 || strlen(reason) > 512 || !out)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   void *conn = db2_conn();
   if (!conn)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   char err[CPL_ERRBUF] = "";
   if (cpl_begin(conn, err, sizeof(err)) != 0)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   int rc = cpl_manifest(conn, "purge", project, "dry_run", PURGE_TARGETS,
                         (int)(sizeof(PURGE_TARGETS) / sizeof(PURGE_TARGETS[0])), out);
   if (rc != 0)
      goto rollback;
   if (strcmp(out->manifest_hash, expected_hash) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_HASH_MISMATCH;
      goto rollback;
   }
   snprintf(out->mode, sizeof(out->mode), "confirmed");
   char detail[CPL_AUDIT_DETAIL_CAP];
   cpl_audit_detail(out, principal, reason, detail, sizeof(detail));
   if (db2_kb_audit_append_in_txn(conn, "operator", principal, "code.index.purge", project, "allow",
                                  detail) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_AUDIT_FAILED;
      goto rollback;
   }
   if (cpl_delete_targets(conn, project, PURGE_TARGETS,
                          (int)(sizeof(PURGE_TARGETS) / sizeof(PURGE_TARGETS[0]))) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_ERROR;
      goto rollback;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_ERROR;
      goto rollback;
   }
   return 0;
rollback:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
}

static void cpl_cutoff(int retention_days, char out[32])
{
   time_t cutoff = time(NULL) - (time_t)(retention_days < 0 ? 0 : retention_days) * 86400;
   struct tm tmv;
   gmtime_r(&cutoff, &tmv);
   /* A day boundary makes a dry-run confirmation token stable during normal
    * operator review.  Crossing UTC midnight changes the criteria/hash and
    * safely requires a fresh dry run. */
   strftime(out, 32, "%Y-%m-%dT00:00:00Z", &tmv);
}

static int cpl_gc_manifest_conn(void *conn, const char *project, int retention_days,
                                code_project_manifest_t *out, char cutoff_out[32])
{
   if (!conn || !project || !project[0] || strlen(project) >= sizeof(out->project) || !out)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   memset(out, 0, sizeof(*out));
   snprintf(out->operation, sizeof(out->operation), "gc");
   snprintf(out->project, sizeof(out->project), "%s", project);
   snprintf(out->mode, sizeof(out->mode), "dry_run");
   int rc = cpl_project_generation(conn, project, &out->generation);
   if (rc != 0)
      return rc;
   char cutoff[32];
   cpl_cutoff(retention_days, cutoff);
   if (cutoff_out)
      snprintf(cutoff_out, 32, "%s", cutoff);
   snprintf(out->criteria, sizeof(out->criteria), "retention_days=%d;cutoff=%s", retention_days,
            cutoff);
   int target_count = (int)(sizeof(GC_TARGETS) / sizeof(GC_TARGETS[0]));
   for (int i = 0; i < target_count; i++)
   {
      if (cpl_measure(conn, &GC_TARGETS[i], project, cutoff, &out->targets[i]) != 0)
         return CODE_PROJECT_LIFECYCLE_ERROR;
      snprintf(out->targets[i].table, sizeof(out->targets[i].table), "%s.retired",
               GC_TARGETS[i].name);
      out->total_rows += out->targets[i].rows;
   }
   out->target_count = target_count;
   return cpl_hash_manifest(out) == 0 ? 0 : CODE_PROJECT_LIFECYCLE_ERROR;
}

int db2_code_project_gc_manifest(const char *project, int retention_days,
                                 code_project_manifest_t *out)
{
   if (retention_days < 0 || retention_days > 3650)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   void *conn = db2_conn();
   if (!conn)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   char err[CPL_ERRBUF] = "";
   if (cpl_begin(conn, err, sizeof(err)) != 0)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   int rc = cpl_gc_manifest_conn(conn, project, retention_days, out, NULL);
   if (rc == 0 && aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) == 0)
      return 0;
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc == CODE_PROJECT_LIFECYCLE_NOT_FOUND ? rc : CODE_PROJECT_LIFECYCLE_ERROR;
}

int db2_code_project_gc_confirm(const char *project, int retention_days, const char *expected_hash,
                                const char *principal, const char *reason,
                                code_project_manifest_t *out)
{
   if (!expected_hash || !expected_hash[0] || !principal || !principal[0] || !reason ||
       !reason[0] || !out)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   if (retention_days < 0 || retention_days > 3650 || strlen(principal) > 575 ||
       strlen(reason) > 512)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   void *conn = db2_conn();
   char err[CPL_ERRBUF] = "";
   if (!conn || cpl_begin(conn, err, sizeof(err)) != 0)
      return CODE_PROJECT_LIFECYCLE_ERROR;
   char cutoff[32];
   int rc = cpl_gc_manifest_conn(conn, project, retention_days, out, cutoff);
   if (rc != 0)
      goto rollback;
   if (strcmp(out->manifest_hash, expected_hash) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_HASH_MISMATCH;
      goto rollback;
   }
   snprintf(out->mode, sizeof(out->mode), "confirmed");
   if (out->total_rows == 0)
   {
      if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
         goto rollback;
      return 0; /* non-mutating no-op needs no destructive-action audit */
   }
   char detail[CPL_AUDIT_DETAIL_CAP];
   cpl_audit_detail(out, principal, reason, detail, sizeof(detail));
   if (db2_kb_audit_append_in_txn(conn, "operator", principal, "code.index.gc", project, "allow",
                                  detail) != 0)
   {
      rc = CODE_PROJECT_LIFECYCLE_AUDIT_FAILED;
      goto rollback;
   }
   int target_count = (int)(sizeof(GC_TARGETS) / sizeof(GC_TARGETS[0]));
   for (int i = 0; i < target_count; i++)
   {
      if (GC_TARGETS[i].optional)
      {
         int exists = cpl_optional_table_exists(conn, &GC_TARGETS[i]);
         if (exists < 0)
            goto rollback_error;
         if (!exists)
            continue;
      }
      char sql[1024];
      snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE %s", GC_TARGETS[i].name,
               GC_TARGETS[i].where_sql);
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
      if (!st)
         goto rollback_error;
      aimee_pg_bind_text(st, "?1", project);
      aimee_pg_bind_text(st, "?2", cutoff);
      if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_DONE)
      {
         aimee_pg_finalize(st);
         goto rollback_error;
      }
      aimee_pg_finalize(st);
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      goto rollback_error;
   return 0;
rollback_error:
   rc = CODE_PROJECT_LIFECYCLE_ERROR;
rollback:
   aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
   return rc;
}
