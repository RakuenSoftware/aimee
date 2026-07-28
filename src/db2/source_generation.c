/* source_generation.c: generation-safe multi-ref source publication. */
#include "source_generation.h"

#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

#define SG_ERRBUF 256
#define SG_DEFAULT_PRUNE_GRACE_SECONDS 604800
#define SG_PRUNE_CLAIM_STALE_SECONDS 900

static int sg_valid_text(const char *s, size_t max)
{
   return s && s[0] && strlen(s) < max && strpbrk(s, "\r\n") == NULL;
}

static int sg_valid_oid(const char *s)
{
   if (!s || (strlen(s) != 40 && strlen(s) != 64))
      return 0;
   for (const char *p = s; *p; p++)
      if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
            (*p >= 'A' && *p <= 'F')))
         return 0;
   return 1;
}

static int sg_exec(void *conn, const char *sql)
{
   char err[SG_ERRBUF] = "";
   return aimee_pg_exec(conn, sql, err, sizeof(err));
}

static void sg_rollback(void *conn)
{
   (void)sg_exec(conn, "ROLLBACK");
}

static int sg_load_generation(void *conn, int64_t generation_id,
                              db2_source_generation_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   static const char *sql =
       "SELECT g.repository_id, g.snapshot_id, g.id, COALESCE(g.project_id,0),"
       " r.repository_key, g.source_ref, s.commit_sha, s.tree_sha,"
       " COALESCE(p.name,''), g.state, COALESCE(sr.refresh_state,''),"
       " COALESCE(sr.is_default, FALSE), g.source_manifest_hash,"
       " g.expected_file_count, g.indexed_file_count,"
       " g.expected_model_subject_count, g.model_subject_count"
       " FROM kb_index_generations g"
       " JOIN kb_source_repositories r ON r.id = g.repository_id"
       " JOIN kb_source_snapshots s ON s.id = g.snapshot_id"
       " LEFT JOIN projects p ON p.id = g.project_id"
       " LEFT JOIN kb_source_refs sr ON sr.repository_id = g.repository_id"
       "  AND sr.ref_name = g.source_ref"
       " WHERE g.id = ?1";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", generation_id);
   int found = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      found = 1;
      if (out)
      {
         out->repository_id = aimee_pg_column_int64(st, 0);
         out->snapshot_id = aimee_pg_column_int64(st, 1);
         out->generation_id = aimee_pg_column_int64(st, 2);
         out->project_id = aimee_pg_column_int64(st, 3);
         snprintf(out->repository_key, sizeof(out->repository_key), "%s",
                  aimee_pg_column_text(st, 4) ? aimee_pg_column_text(st, 4) : "");
         snprintf(out->source_ref, sizeof(out->source_ref), "%s",
                  aimee_pg_column_text(st, 5) ? aimee_pg_column_text(st, 5) : "");
         snprintf(out->commit_sha, sizeof(out->commit_sha), "%s",
                  aimee_pg_column_text(st, 6) ? aimee_pg_column_text(st, 6) : "");
         snprintf(out->tree_sha, sizeof(out->tree_sha), "%s",
                  aimee_pg_column_text(st, 7) ? aimee_pg_column_text(st, 7) : "");
         snprintf(out->physical_project, sizeof(out->physical_project), "%s",
                  aimee_pg_column_text(st, 8) ? aimee_pg_column_text(st, 8) : "");
         snprintf(out->state, sizeof(out->state), "%s",
                  aimee_pg_column_text(st, 9) ? aimee_pg_column_text(st, 9) : "");
         snprintf(out->refresh_state, sizeof(out->refresh_state), "%s",
                  aimee_pg_column_text(st, 10) ? aimee_pg_column_text(st, 10) : "");
         out->is_default = aimee_pg_column_int(st, 11) ? 1 : 0;
         snprintf(out->source_manifest_hash, sizeof(out->source_manifest_hash), "%s",
                  aimee_pg_column_text(st, 12) ? aimee_pg_column_text(st, 12) : "");
         out->expected_file_count = aimee_pg_column_int64(st, 13);
         out->indexed_file_count = aimee_pg_column_int64(st, 14);
         out->expected_model_subject_count = aimee_pg_column_int64(st, 15);
         out->model_subject_count = aimee_pg_column_int64(st, 16);
      }
   }
   aimee_pg_finalize(st);
   return found;
}

static int64_t sg_upsert_repository(void *conn, const char *key, const char *default_ref)
{
   /* Repositories are unique per tenant, so the conflict target is the full
    * (team_id, repository_key) key. team_id stays 0 until the write path carries
    * an authenticated team; the identity of the key is what matters here. */
   static const char *sql =
       "INSERT INTO kb_source_repositories (repository_key, team_id, default_ref, updated_at)"
       " VALUES (?1, 0, ?2, pg_now_text())"
       " ON CONFLICT (team_id, repository_key) DO UPDATE SET"
       " default_ref = CASE WHEN EXCLUDED.default_ref = '' THEN kb_source_repositories.default_ref"
       "                    ELSE EXCLUDED.default_ref END, updated_at = pg_now_text()"
       " RETURNING id";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", key);
   aimee_pg_bind_text(st, "?2", default_ref ? default_ref : "");
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int64_t sg_upsert_snapshot(void *conn, int64_t repository_id, const char *commit_sha,
                                  const char *tree_sha)
{
   static const char *sql =
       "INSERT INTO kb_source_snapshots"
       " (repository_id, snapshot_kind, commit_sha, tree_sha, worktree_hash)"
       " VALUES (?1, 'commit', ?2, ?3, '')"
       " ON CONFLICT (repository_id, snapshot_kind, commit_sha, tree_sha, worktree_hash)"
       " DO UPDATE SET commit_sha = kb_source_snapshots.commit_sha RETURNING id";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", repository_id);
   aimee_pg_bind_text(st, "?2", commit_sha);
   aimee_pg_bind_text(st, "?3", tree_sha);
   int64_t id = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

static int sg_upsert_observed_ref(void *conn, int64_t repository_id, const char *source_ref,
                                  int is_default, const char *commit_sha, const char *tree_sha,
                                  const char *refresh_state)
{
   char err[SG_ERRBUF] = "";
   if (is_default)
   {
      aimee_pg_stmt_t *clear = aimee_pg_prepare(
          conn, "UPDATE kb_source_refs SET is_default = FALSE WHERE repository_id = ?1", err,
          sizeof(err));
      if (!clear)
         return -1;
      aimee_pg_bind_int64(clear, "?1", repository_id);
      (void)aimee_pg_step(clear, err, sizeof(err));
      aimee_pg_finalize(clear);
   }
   static const char *sql =
       "INSERT INTO kb_source_refs"
       " (repository_id, ref_name, is_default, observed_commit_sha, observed_tree_sha,"
       "  lifecycle_state, retired_at, retirement_reason, refresh_state, last_observed_at,"
       "  last_error)"
       " VALUES (?1, ?2, ?3, ?4, ?5, 'active', '', '', ?6, pg_now_text(), '')"
       " ON CONFLICT (repository_id, ref_name) DO UPDATE SET"
       " is_default = EXCLUDED.is_default, observed_commit_sha = EXCLUDED.observed_commit_sha,"
       " observed_tree_sha = EXCLUDED.observed_tree_sha, refresh_state = EXCLUDED.refresh_state,"
       " lifecycle_state='active', retired_at='', retirement_reason='',"
       " last_observed_at = pg_now_text(), last_error = ''";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", repository_id);
   aimee_pg_bind_text(st, "?2", source_ref);
   aimee_pg_bind_int(st, "?3", is_default ? 1 : 0);
   aimee_pg_bind_text(st, "?4", commit_sha);
   aimee_pg_bind_text(st, "?5", tree_sha);
   aimee_pg_bind_text(st, "?6", refresh_state);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return ok;
}

static int64_t sg_find_snapshot_generation(void *conn, int64_t snapshot_id, int published,
                                           char *state_out, size_t state_cap)
{
   const char *sql = published
                         ? "SELECT id, state FROM kb_index_generations"
                           " WHERE snapshot_id = ?1 AND state IN ('published','superseded')"
                           " ORDER BY CASE WHEN state='published' THEN 0 ELSE 1 END, id DESC LIMIT 1"
                         : "SELECT id, state FROM kb_index_generations"
                           " WHERE snapshot_id = ?1"
                           " AND state IN ('staging','encoding','validating')"
                           " ORDER BY id DESC LIMIT 1";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", snapshot_id);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      id = aimee_pg_column_int64(st, 0);
      if (state_out && state_cap)
         snprintf(state_out, state_cap, "%s",
                  aimee_pg_column_text(st, 1) ? aimee_pg_column_text(st, 1) : "");
   }
   aimee_pg_finalize(st);
   return id;
}

static int64_t sg_ref_active_generation(void *conn, int64_t repository_id,
                                        const char *source_ref)
{
   static const char *sql =
       "SELECT COALESCE(active_generation_id,0) FROM kb_source_refs"
       " WHERE repository_id=?1 AND ref_name=?2";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", repository_id);
   aimee_pg_bind_text(st, "?2", source_ref);
   int64_t id = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return id;
}

/* A generation is superseded only after every moving ref has left it. The
 * immutable snapshot/project remain available for history and can be promoted
 * back to published if a branch later returns to the same commit. */
static int sg_retire_unreferenced(void *conn, int64_t repository_id,
                                  int64_t keep_generation_id)
{
   static const char *sql =
       "UPDATE kb_index_generations SET state='superseded',"
       " prune_after=CASE WHEN prune_after='' THEN"
       " pg_now_text(CAST(COALESCE((SELECT prune_grace_seconds"
       " FROM kb_source_repositories WHERE id=?1),?3) AS TEXT) || ' seconds')"
       " ELSE prune_after END"
       " WHERE repository_id=?1 AND state='published' AND id<>?2"
       " AND NOT EXISTS (SELECT 1 FROM kb_source_refs sr"
       "                 WHERE sr.active_generation_id=kb_index_generations.id"
       "                   AND sr.lifecycle_state='active')";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", repository_id);
   aimee_pg_bind_int64(st, "?2", keep_generation_id);
   aimee_pg_bind_int(st, "?3", SG_DEFAULT_PRUNE_GRACE_SECONDS);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   return ok ? 0 : -1;
}

static int sg_point_ref(void *conn, int64_t repository_id, const char *source_ref,
                        int64_t generation_id, const char *state)
{
   static const char *sql =
       "UPDATE kb_source_refs SET active_generation_id = ?1, refresh_state = ?2,"
       " last_published_at = CASE WHEN ?2 = 'current' THEN pg_now_text() ELSE last_published_at END,"
       " last_error = '' WHERE repository_id = ?3 AND ref_name = ?4";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   if (generation_id > 0)
      aimee_pg_bind_int64(st, "?1", generation_id);
   else
      aimee_pg_bind_null(st, "?1");
   aimee_pg_bind_text(st, "?2", state);
   aimee_pg_bind_int64(st, "?3", repository_id);
   aimee_pg_bind_text(st, "?4", source_ref);
   int rc = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE ? 0 : -1;
   aimee_pg_finalize(st);
   return rc;
}

int db2_source_generation_begin(const char *repository_key, const char *source_ref,
                                int is_default, const char *commit_sha, const char *tree_sha,
                                const char *root_label, db2_source_generation_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!sg_valid_text(repository_key, 512) || !sg_valid_text(source_ref, 1024) ||
       !sg_valid_oid(commit_sha) || !sg_valid_oid(tree_sha))
      return -1;
   void *conn = db2_conn();
   if (!conn || sg_exec(conn, "BEGIN") != 0)
      return -1;

   int64_t repository_id =
       sg_upsert_repository(conn, repository_key, is_default ? source_ref : "");
   int64_t prior_active =
       repository_id > 0 ? sg_ref_active_generation(conn, repository_id, source_ref) : -1;
   int64_t snapshot_id =
       repository_id > 0 ? sg_upsert_snapshot(conn, repository_id, commit_sha, tree_sha) : -1;
   if (snapshot_id <= 0 ||
       sg_upsert_observed_ref(conn, repository_id, source_ref, is_default, commit_sha, tree_sha,
                              "refreshing") != 0)
   {
      sg_rollback(conn);
      return -1;
   }

   char state[32] = "";
   int64_t generation_id = sg_find_snapshot_generation(conn, snapshot_id, 1, state, sizeof(state));
   if (generation_id > 0)
   {
      char err[SG_ERRBUF] = "";
      aimee_pg_stmt_t *activate = aimee_pg_prepare(
          conn, "UPDATE kb_index_generations SET state='published', prune_after='',"
                " prune_source_state='' WHERE id=?1", err,
          sizeof(err));
      if (!activate)
      {
         sg_rollback(conn);
         return -1;
      }
      aimee_pg_bind_int64(activate, "?1", generation_id);
      int activated = aimee_pg_step(activate, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(activate);
      if (!activated ||
          sg_point_ref(conn, repository_id, source_ref, generation_id, "current") != 0 ||
          sg_retire_unreferenced(conn, repository_id, generation_id) != 0 ||
          sg_exec(conn, "COMMIT") != 0)
      {
         sg_rollback(conn);
         return -1;
      }
      int rc = sg_load_generation(conn, generation_id, out);
      if (rc == 1 && out)
      {
         out->already_current = prior_active == generation_id ? 1 : 0;
         out->reused_snapshot = 1;
         snprintf(out->source_ref, sizeof(out->source_ref), "%s", source_ref);
         snprintf(out->refresh_state, sizeof(out->refresh_state), "current");
         out->is_default = is_default ? 1 : 0;
      }
      return rc == 1 ? 0 : -1;
   }

   generation_id = sg_find_snapshot_generation(conn, snapshot_id, 0, state, sizeof(state));
   if (generation_id > 0)
   {
      if (sg_exec(conn, "COMMIT") != 0)
      {
         sg_rollback(conn);
         return -1;
      }
      int rc = sg_load_generation(conn, generation_id, out);
      if (rc == 1 && out)
      {
         out->reused_snapshot = 1;
         snprintf(out->source_ref, sizeof(out->source_ref), "%s", source_ref);
         out->is_default = is_default ? 1 : 0;
      }
      return rc == 1 ? 0 : -1;
   }

   char err[SG_ERRBUF] = "";
   static const char *generation_sql =
       "INSERT INTO kb_index_generations"
       " (surface, repository_id, snapshot_id, source_ref, state, graph_schema_version,"
       "  representation_schema_version, last_progress_at)"
       " VALUES ('code', ?1, ?2, ?3, 'staging', 'aimee-code-graph-v1',"
       "  'aimee-kb-native-v1', pg_now_text()) RETURNING id";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, generation_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", repository_id);
   aimee_pg_bind_int64(st, "?2", snapshot_id);
   aimee_pg_bind_text(st, "?3", source_ref);
   generation_id = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW
                       ? aimee_pg_column_int64(st, 0)
                       : -1;
   aimee_pg_finalize(st);
   if (generation_id <= 0)
   {
      sg_rollback(conn);
      return -1;
   }

   char physical[128];
   snprintf(physical, sizeof(physical), "generation:%lld:%lld", (long long)repository_id,
            (long long)generation_id);
   static const char *project_sql =
       "INSERT INTO projects"
       " (name, root, scanned_at, repository_id, source_snapshot_id, index_generation_id,"
       "  source_ref, source_commit, source_tree, snapshot_kind)"
       " VALUES (?1, ?2, pg_now_text(), ?3, ?4, ?5, ?6, ?7, ?8, 'commit') RETURNING id";
   st = aimee_pg_prepare(conn, project_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", physical);
   aimee_pg_bind_text(st, "?2", root_label && root_label[0] ? root_label : "remote");
   aimee_pg_bind_int64(st, "?3", repository_id);
   aimee_pg_bind_int64(st, "?4", snapshot_id);
   aimee_pg_bind_int64(st, "?5", generation_id);
   aimee_pg_bind_text(st, "?6", source_ref);
   aimee_pg_bind_text(st, "?7", commit_sha);
   aimee_pg_bind_text(st, "?8", tree_sha);
   int64_t project_id = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW
                            ? aimee_pg_column_int64(st, 0)
                            : -1;
   aimee_pg_finalize(st);
   if (project_id <= 0)
   {
      sg_rollback(conn);
      return -1;
   }

   st = aimee_pg_prepare(conn,
                         "UPDATE kb_index_generations SET project_id = ?1 WHERE id = ?2", err,
                         sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_int64(st, "?2", generation_id);
   int update_ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   if (!update_ok || sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   return sg_load_generation(conn, generation_id, out) == 1 ? 0 : -1;
}

int db2_source_generation_source_complete(int64_t generation_id,
                                          const char *source_manifest_hash,
                                          int64_t expected_file_count,
                                          db2_source_generation_t *out)
{
   if (generation_id <= 0 || !sg_valid_oid(source_manifest_hash) || expected_file_count < 0)
      return -1;
   void *conn = db2_conn();
   db2_source_generation_t row;
   if (!conn || sg_load_generation(conn, generation_id, &row) != 1 || row.project_id <= 0 ||
       strcmp(row.state, "staging") != 0)
      return -1;

   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT COUNT(*) FROM files WHERE project_id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", row.project_id);
   int64_t actual = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      actual = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   if (actual != expected_file_count)
   {
      st = aimee_pg_prepare(
          conn,
          "UPDATE kb_index_generations SET expected_file_count=?1, indexed_file_count=?2,"
          " failure_reason='source manifest coverage mismatch', last_progress_at=pg_now_text()"
          " WHERE id=?3",
          err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", expected_file_count);
         aimee_pg_bind_int64(st, "?2", actual);
         aimee_pg_bind_int64(st, "?3", generation_id);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
      return -1;
   }

   static const char *sql =
       "UPDATE kb_index_generations SET source_manifest_hash=?1, source_complete=TRUE,"
       " graph_complete=TRUE, expected_file_count=?2, indexed_file_count=?3, state='encoding',"
       " failure_reason='', last_progress_at=pg_now_text() WHERE id=?4 AND state='staging'";
   st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", source_manifest_hash);
   aimee_pg_bind_int64(st, "?2", expected_file_count);
   aimee_pg_bind_int64(st, "?3", actual);
   aimee_pg_bind_int64(st, "?4", generation_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
            aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   return ok && sg_load_generation(conn, generation_id, out) == 1 ? 0 : -1;
}

int db2_source_generation_model_complete(int64_t generation_id,
                                         int64_t expected_subject_count,
                                         int64_t actual_subject_count, const char *model_id,
                                         const char *checkpoint_hash,
                                         const char *tokenizer_hash,
                                         db2_source_generation_t *out)
{
   if (generation_id <= 0 || expected_subject_count < 0 ||
       actual_subject_count < expected_subject_count)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *sql =
       "UPDATE kb_index_generations SET model_complete=TRUE, expected_model_subject_count=?1,"
       " model_subject_count=?2, model_id=?3, checkpoint_hash=?4, tokenizer_hash=?5,"
       " state='validating', failure_reason='', last_progress_at=pg_now_text()"
       " WHERE id=?6 AND state='encoding' AND source_complete=TRUE AND graph_complete=TRUE";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", expected_subject_count);
   aimee_pg_bind_int64(st, "?2", actual_subject_count);
   aimee_pg_bind_text(st, "?3", model_id ? model_id : "");
   aimee_pg_bind_text(st, "?4", checkpoint_hash ? checkpoint_hash : "");
   aimee_pg_bind_text(st, "?5", tokenizer_hash ? tokenizer_hash : "");
   aimee_pg_bind_int64(st, "?6", generation_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
            aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   return ok && sg_load_generation(conn, generation_id, out) == 1 ? 0 : -1;
}

int db2_source_generation_publish(int64_t generation_id, db2_source_generation_t *out)
{
   if (generation_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn || sg_exec(conn, "BEGIN") != 0)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *sql =
       "SELECT g.repository_id, g.snapshot_id, g.source_ref, g.state, g.source_complete,"
       " g.graph_complete, g.model_complete, s.commit_sha, s.tree_sha,"
       " sr.observed_commit_sha, sr.observed_tree_sha, COALESCE(sr.active_generation_id,0),"
       " sr.lifecycle_state"
       " FROM kb_index_generations g"
       " JOIN kb_source_snapshots s ON s.id=g.snapshot_id"
       " JOIN kb_source_refs sr ON sr.repository_id=g.repository_id AND sr.ref_name=g.source_ref"
       " WHERE g.id=?1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", generation_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      sg_rollback(conn);
      return -1;
   }
   int64_t repository_id = aimee_pg_column_int64(st, 0);
   const char *source_ref_v = aimee_pg_column_text(st, 2);
   const char *state = aimee_pg_column_text(st, 3);
   int source_complete = aimee_pg_column_int(st, 4);
   int graph_complete = aimee_pg_column_int(st, 5);
   int model_complete = aimee_pg_column_int(st, 6);
   const char *commit = aimee_pg_column_text(st, 7);
   const char *tree = aimee_pg_column_text(st, 8);
   const char *observed_commit = aimee_pg_column_text(st, 9);
   const char *observed_tree = aimee_pg_column_text(st, 10);
   int64_t old_generation = aimee_pg_column_int64(st, 11);
   const char *lifecycle = aimee_pg_column_text(st, 12);
   char source_ref[1024], commit_copy[65], tree_copy[65], observed_commit_copy[65],
       observed_tree_copy[65], lifecycle_copy[32];
   snprintf(source_ref, sizeof(source_ref), "%s", source_ref_v ? source_ref_v : "");
   snprintf(commit_copy, sizeof(commit_copy), "%s", commit ? commit : "");
   snprintf(tree_copy, sizeof(tree_copy), "%s", tree ? tree : "");
   snprintf(observed_commit_copy, sizeof(observed_commit_copy), "%s",
            observed_commit ? observed_commit : "");
   snprintf(observed_tree_copy, sizeof(observed_tree_copy), "%s", observed_tree ? observed_tree : "");
   snprintf(lifecycle_copy, sizeof(lifecycle_copy), "%s", lifecycle ? lifecycle : "");
   int ready = state && strcmp(state, "validating") == 0 && source_complete && graph_complete &&
               model_complete;
   aimee_pg_finalize(st);
   if (!ready)
   {
      sg_rollback(conn);
      return -1;
   }
   if (strcmp(lifecycle_copy, "active") != 0 || strcmp(commit_copy, observed_commit_copy) != 0 ||
       strcmp(tree_copy, observed_tree_copy) != 0)
   {
      const char *publication_error = strcmp(lifecycle_copy, "active") != 0
                                          ? "ref retired before publication"
                                          : "ref moved before publication";
      st = aimee_pg_prepare(
          conn,
          "UPDATE kb_index_generations SET state='aborted',"
          " failure_reason=?2, prune_after=pg_now_text(CAST(COALESCE((SELECT"
          " prune_grace_seconds FROM kb_source_repositories WHERE id=?3),?4) AS TEXT)"
          " || ' seconds'), last_progress_at=pg_now_text() WHERE id=?1",
          err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", generation_id);
         aimee_pg_bind_text(st, "?2", publication_error);
         aimee_pg_bind_int64(st, "?3", repository_id);
         aimee_pg_bind_int(st, "?4", SG_DEFAULT_PRUNE_GRACE_SECONDS);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
      st = aimee_pg_prepare(
          conn,
          "UPDATE kb_source_refs SET refresh_state='pending',"
          " last_error=?3 WHERE repository_id=?1 AND ref_name=?2"
          " AND lifecycle_state='active'",
          err, sizeof(err));
      if (st)
      {
         aimee_pg_bind_int64(st, "?1", repository_id);
         aimee_pg_bind_text(st, "?2", source_ref);
         aimee_pg_bind_text(st, "?3", publication_error);
         (void)aimee_pg_step(st, err, sizeof(err));
         aimee_pg_finalize(st);
      }
      if (sg_exec(conn, "COMMIT") != 0)
         sg_rollback(conn);
      return -2;
   }

   st = aimee_pg_prepare(
       conn,
       "UPDATE kb_index_generations SET state='published', published_at=pg_now_text(),"
       " prune_after='', prune_source_state='', last_progress_at=pg_now_text()"
       " WHERE id=?1 AND state='validating'",
       err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", generation_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
            aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   if (!ok)
   {
      sg_rollback(conn);
      return -1;
   }

   /* Publish to every observed ref still naming these exact immutable bytes.
    * This is what makes two branches at the same commit share one encoding
    * generation without either branch being silently left on stale evidence. */
   st = aimee_pg_prepare(
       conn,
       "UPDATE kb_source_refs SET active_generation_id=?1, refresh_state='current',"
       " last_published_at=pg_now_text(), last_error=''"
       " WHERE repository_id=?2 AND observed_commit_sha=?3 AND observed_tree_sha=?4"
       " AND lifecycle_state='active'",
       err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", generation_id);
   aimee_pg_bind_int64(st, "?2", repository_id);
   aimee_pg_bind_text(st, "?3", commit_copy);
   aimee_pg_bind_text(st, "?4", tree_copy);
   ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
        aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   if (!ok || sg_retire_unreferenced(conn, repository_id, generation_id) != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   (void)old_generation;
   if (sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   return sg_load_generation(conn, generation_id, out) == 1 ? 0 : -1;
}

int db2_source_generation_abort(int64_t generation_id, const char *reason)
{
   if (generation_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   db2_source_generation_t row;
   if (sg_load_generation(conn, generation_id, &row) != 1 ||
       sg_exec(conn, "BEGIN") != 0)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *sql =
       "UPDATE kb_index_generations SET state='aborted', failure_reason=?1,"
       " prune_after=pg_now_text(CAST(COALESCE((SELECT prune_grace_seconds"
       " FROM kb_source_repositories WHERE id=kb_index_generations.repository_id),?3) AS TEXT)"
       " || ' seconds'), last_progress_at=pg_now_text() WHERE id=?2"
       " AND state IN ('staging','encoding','validating')";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", reason ? reason : "aborted");
   aimee_pg_bind_int64(st, "?2", generation_id);
   aimee_pg_bind_int(st, "?3", SG_DEFAULT_PRUNE_GRACE_SECONDS);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
            aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   if (!ok)
   {
      sg_rollback(conn);
      return -1;
   }
   st = aimee_pg_prepare(
       conn,
       "UPDATE kb_source_refs SET refresh_state='pending', last_error=?1"
       " WHERE repository_id=?2 AND observed_commit_sha=?3 AND observed_tree_sha=?4",
       err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", reason ? reason : "aborted");
   aimee_pg_bind_int64(st, "?2", row.repository_id);
   aimee_pg_bind_text(st, "?3", row.commit_sha);
   aimee_pg_bind_text(st, "?4", row.tree_sha);
   ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   if (!ok || sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   return 0;
}

int db2_source_generation_get(int64_t generation_id, db2_source_generation_t *out)
{
   if (generation_id <= 0)
      return -1;
   void *conn = db2_conn();
   return conn ? sg_load_generation(conn, generation_id, out) : -1;
}

int db2_source_generation_link_file_evidence(int64_t generation_id, const char *rel_path,
                                             int64_t original_version_id)
{
   if (generation_id <= 0 || !sg_valid_text(rel_path, 4096) || original_version_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "UPDATE files SET original_version_id=?1, index_generation_id=?2,"
       " evidence_authority='secondary_source_anchored_code'"
       " WHERE project_id=(SELECT project_id FROM kb_index_generations"
       "                   WHERE id=?2 AND state='staging') AND path=?3";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", original_version_id);
   aimee_pg_bind_int64(st, "?2", generation_id);
   aimee_pg_bind_text(st, "?3", rel_path);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      rc = aimee_pg_stmt_changes(st) > 0 ? 1 : 0;
   aimee_pg_finalize(st);
   return rc;
}

int db2_source_generation_record_embedding_lineage(int64_t generation_id,
                                                    const char *model_id)
{
   if (generation_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *insert_sql =
       "INSERT INTO kb_evidence_lineage"
       " (subject_kind,subject_id,evidence_tier,original_version_id,"
       "  parent_evidence_kind,parent_evidence_id,source_anchors,derivation_kind,"
       "  derivation_hash,model_id,index_generation_id)"
       " SELECT 'code_embedding',CAST(ce.point_id AS TEXT),'tertiary',f.original_version_id,"
       "        'code_file',CAST(f.id AS TEXT),'[]','model_embedding',ce.content_hash,?1,?2"
       " FROM kb_index_generations g"
       " JOIN files f ON f.project_id=g.project_id"
       " JOIN projects p ON p.id=f.project_id"
       " JOIN code_embeddings ce ON ce.point_id=f.id AND ce.project=p.name"
       " WHERE g.id=?2 AND g.state='encoding' AND f.original_version_id IS NOT NULL"
       "   AND ce.source_hash=f.hash ON CONFLICT DO NOTHING";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, insert_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", model_id ? model_id : "");
   aimee_pg_bind_int64(st, "?2", generation_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   if (!ok)
      return -1;

   static const char *count_sql =
       "SELECT COUNT(DISTINCT f.id) FROM kb_index_generations g"
       " JOIN files f ON f.project_id=g.project_id"
       " JOIN projects p ON p.id=f.project_id"
       " JOIN code_embeddings ce ON ce.point_id=f.id AND ce.project=p.name"
       " WHERE g.id=?1 AND f.original_version_id IS NOT NULL AND ce.source_hash=f.hash";
   st = aimee_pg_prepare(conn, count_sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", generation_id);
   int covered = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      covered = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return covered;
}

int db2_source_ref_resolve_current(const char *repository_key, const char *source_ref,
                                   db2_source_generation_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!sg_valid_text(repository_key, 512))
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "SELECT g.id, sr.ref_name, sr.refresh_state, sr.is_default"
       " FROM kb_source_repositories r"
       " JOIN kb_source_refs sr ON sr.repository_id=r.id"
       " JOIN kb_index_generations g ON g.id=sr.active_generation_id AND g.state='published'"
       " WHERE r.repository_key=?1"
       " AND sr.ref_name=CASE WHEN ?2='' THEN r.default_ref ELSE ?2 END"
       " AND sr.lifecycle_state='active' AND sr.refresh_state='current' LIMIT 1";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", repository_key);
   aimee_pg_bind_text(st, "?2", source_ref ? source_ref : "");
   int64_t generation_id = 0;
   char resolved_ref[1024] = "";
   char refresh_state[32] = "";
   int is_default = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      generation_id = aimee_pg_column_int64(st, 0);
      snprintf(resolved_ref, sizeof(resolved_ref), "%s",
               aimee_pg_column_text(st, 1) ? aimee_pg_column_text(st, 1) : "");
      snprintf(refresh_state, sizeof(refresh_state), "%s",
               aimee_pg_column_text(st, 2) ? aimee_pg_column_text(st, 2) : "");
      is_default = aimee_pg_column_int(st, 3) ? 1 : 0;
   }
   aimee_pg_finalize(st);
   if (generation_id <= 0)
      return 0;
   int rc = sg_load_generation(conn, generation_id, out);
   if (rc == 1 && out)
   {
      snprintf(out->source_ref, sizeof(out->source_ref), "%s", resolved_ref);
      snprintf(out->refresh_state, sizeof(out->refresh_state), "%s", refresh_state);
      out->is_default = is_default;
   }
   return rc;
}

int db2_source_ref_retire(const char *repository_key, const char *source_ref,
                          const char *reason, int grace_seconds)
{
   if (!sg_valid_text(repository_key, 512) || !sg_valid_text(source_ref, 1024) ||
       !reason || (strcmp(reason, "merged") != 0 && strcmp(reason, "deleted") != 0) ||
       grace_seconds < -1)
      return -1;
   void *conn = db2_conn();
   if (!conn || sg_exec(conn, "BEGIN") != 0)
      return -1;

   char err[SG_ERRBUF] = "";
   static const char *load_sql =
       "SELECT r.id, COALESCE(sr.active_generation_id,0), r.prune_grace_seconds"
       " FROM kb_source_repositories r JOIN kb_source_refs sr ON sr.repository_id=r.id"
       " WHERE r.repository_key=?1 AND sr.ref_name=?2";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, load_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", repository_key);
   aimee_pg_bind_text(st, "?2", source_ref);
   int step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      if (step != AIMEE_PG_DONE || sg_exec(conn, "COMMIT") != 0)
      {
         sg_rollback(conn);
         return -1;
      }
      return 0;
   }
   int64_t repository_id = aimee_pg_column_int64(st, 0);
   int64_t old_generation_id = aimee_pg_column_int64(st, 1);
   int repository_grace = aimee_pg_column_int(st, 2);
   aimee_pg_finalize(st);
   int effective_grace = grace_seconds >= 0 ? grace_seconds : repository_grace;
   if (effective_grace < 0)
      effective_grace = SG_DEFAULT_PRUNE_GRACE_SECONDS;

   static const char *retire_sql =
       "UPDATE kb_source_refs SET active_generation_id=NULL, lifecycle_state=?1,"
       " retired_at=pg_now_text(), retirement_reason=?1, refresh_state='stale',"
       " last_error='' WHERE repository_id=?2 AND ref_name=?3";
   st = aimee_pg_prepare(conn, retire_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_text(st, "?1", reason);
   aimee_pg_bind_int64(st, "?2", repository_id);
   aimee_pg_bind_text(st, "?3", source_ref);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
            aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   if (!ok)
   {
      sg_rollback(conn);
      return -1;
   }

   if (old_generation_id > 0)
   {
      static const char *schedule_sql =
          "UPDATE kb_index_generations SET"
          " state=CASE WHEN state='published' THEN 'superseded' ELSE state END,"
          " prune_after=pg_now_text(CAST(?1 AS TEXT) || ' seconds')"
          " WHERE id=?2 AND state IN ('published','superseded','aborted')"
          " AND NOT EXISTS (SELECT 1 FROM kb_source_refs sr"
          " WHERE sr.active_generation_id=kb_index_generations.id"
          " AND sr.lifecycle_state='active')";
      st = aimee_pg_prepare(conn, schedule_sql, err, sizeof(err));
      if (!st)
      {
         sg_rollback(conn);
         return -1;
      }
      aimee_pg_bind_int(st, "?1", effective_grace);
      aimee_pg_bind_int64(st, "?2", old_generation_id);
      ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
      aimee_pg_finalize(st);
   }
   if (!ok || sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   return 1;
}

int db2_source_generation_prune_claim(db2_source_prune_candidate_t *out)
{
   if (out)
      memset(out, 0, sizeof(*out));
   if (!out)
      return -1;
   void *conn = db2_conn();
   if (!conn || sg_exec(conn, "BEGIN") != 0)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *select_sql =
       "SELECT g.id,g.repository_id,g.project_id,r.repository_key,p.name,g.state,"
       " g.prune_source_state FROM kb_index_generations g"
       " JOIN kb_source_repositories r ON r.id=g.repository_id"
       " JOIN projects p ON p.id=g.project_id"
       " WHERE ((g.state IN ('superseded','aborted') AND g.prune_after<>''"
       "         AND g.prune_after<=pg_now_text())"
       " OR (g.state='pruning' AND g.last_progress_at<="
       "     pg_now_text('-900 seconds')))"
       " AND NOT EXISTS (SELECT 1 FROM kb_source_refs sr"
       " WHERE sr.active_generation_id=g.id AND sr.lifecycle_state='active')"
       " ORDER BY CASE WHEN g.state='pruning' THEN 0 ELSE 1 END,g.prune_after,g.id LIMIT 1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, select_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   int step = aimee_pg_step(st, err, sizeof(err));
   if (step != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      if (step != AIMEE_PG_DONE || sg_exec(conn, "COMMIT") != 0)
      {
         sg_rollback(conn);
         return -1;
      }
      return 0;
   }
   out->generation_id = aimee_pg_column_int64(st, 0);
   out->repository_id = aimee_pg_column_int64(st, 1);
   out->project_id = aimee_pg_column_int64(st, 2);
   snprintf(out->repository_key, sizeof(out->repository_key), "%s",
            aimee_pg_column_text(st, 3) ? aimee_pg_column_text(st, 3) : "");
   snprintf(out->physical_project, sizeof(out->physical_project), "%s",
            aimee_pg_column_text(st, 4) ? aimee_pg_column_text(st, 4) : "");
   const char *state = aimee_pg_column_text(st, 5);
   const char *resume = aimee_pg_column_text(st, 6);
   snprintf(out->prior_state, sizeof(out->prior_state), "%s",
            state && strcmp(state, "pruning") == 0 ? (resume ? resume : "")
                                                    : (state ? state : ""));
   aimee_pg_finalize(st);

   static const char *claim_sql =
       "UPDATE kb_index_generations SET"
       " prune_source_state=CASE WHEN state='pruning' THEN prune_source_state ELSE state END,"
       " state='pruning',last_progress_at=pg_now_text() WHERE id=?1"
       " AND (state IN ('superseded','aborted') OR"
       " (state='pruning' AND last_progress_at<=pg_now_text('-900 seconds')))"
       " AND NOT EXISTS (SELECT 1 FROM kb_source_refs sr"
       " WHERE sr.active_generation_id=kb_index_generations.id"
       " AND sr.lifecycle_state='active')";
   st = aimee_pg_prepare(conn, claim_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", out->generation_id);
   int claimed = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
                 aimee_pg_stmt_changes(st) > 0;
   aimee_pg_finalize(st);
   if (!claimed || sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      memset(out, 0, sizeof(*out));
      return claimed ? -1 : 0;
   }
   return 1;
}

int db2_source_generation_prune_finalize(int64_t generation_id)
{
   if (generation_id <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn || sg_exec(conn, "BEGIN") != 0)
      return -1;
   char err[SG_ERRBUF] = "";
   static const char *load_sql =
       "SELECT g.snapshot_id,g.project_id,p.name FROM kb_index_generations g"
       " JOIN projects p ON p.id=g.project_id WHERE g.id=?1 AND g.state='pruning'"
       " AND NOT EXISTS (SELECT 1 FROM kb_source_refs sr"
       " WHERE sr.active_generation_id=g.id AND sr.lifecycle_state='active')";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, load_sql, err, sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", generation_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      sg_rollback(conn);
      return 0;
   }
   int64_t snapshot_id = aimee_pg_column_int64(st, 0);
   int64_t project_id = aimee_pg_column_int64(st, 1);
   char physical_project[128];
   snprintf(physical_project, sizeof(physical_project), "%s",
            aimee_pg_column_text(st, 2) ? aimee_pg_column_text(st, 2) : "");
   aimee_pg_finalize(st);

   /* Remove generation-scoped derivations before the project cascade removes
    * the generation row. Primary originals go last, after files no longer hold
    * version FKs. External vector stores have already been purged by caller. */
   st = aimee_pg_prepare(conn, "DELETE FROM artifacts WHERE index_generation_id=?1", err,
                         sizeof(err));
   if (!st)
   {
      sg_rollback(conn);
      return -1;
   }
   aimee_pg_bind_int64(st, "?1", generation_id);
   int ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
   aimee_pg_finalize(st);
   if (ok)
   {
      st = aimee_pg_prepare(conn, "DELETE FROM kb_evidence_lineage"
                                  " WHERE index_generation_id=?1",
                            err, sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_int64(st, "?1", generation_id);
         ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
         aimee_pg_finalize(st);
      }
   }
   if (ok)
   {
      st = aimee_pg_prepare(conn, "DELETE FROM kb_documents WHERE project=?1", err,
                            sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_text(st, "?1", physical_project);
         ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
         aimee_pg_finalize(st);
      }
   }
   if (ok)
   {
      st = aimee_pg_prepare(conn, "DELETE FROM projects WHERE id=?1", err, sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_int64(st, "?1", project_id);
         ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE &&
              aimee_pg_stmt_changes(st) > 0;
         aimee_pg_finalize(st);
      }
   }
   if (ok)
   {
      st = aimee_pg_prepare(conn, "DELETE FROM kb_original_documents WHERE project=?1", err,
                            sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_text(st, "?1", physical_project);
         ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
         aimee_pg_finalize(st);
      }
   }
   if (ok)
   {
      st = aimee_pg_prepare(
          conn,
          "DELETE FROM kb_source_snapshots WHERE id=?1 AND NOT EXISTS"
          " (SELECT 1 FROM kb_index_generations g WHERE g.snapshot_id=?1)",
          err, sizeof(err));
      if (!st)
         ok = 0;
      else
      {
         aimee_pg_bind_int64(st, "?1", snapshot_id);
         ok = aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE;
         aimee_pg_finalize(st);
      }
   }
   if (!ok || sg_exec(conn, "COMMIT") != 0)
   {
      sg_rollback(conn);
      return -1;
   }
   return 1;
}

int db2_source_generation_prune_release(int64_t generation_id, const char *reason,
                                        int retry_seconds)
{
   if (generation_id <= 0 || retry_seconds < 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql =
       "UPDATE kb_index_generations SET"
       " state=CASE WHEN prune_source_state IN ('superseded','aborted')"
       " THEN prune_source_state ELSE 'superseded' END,"
       " failure_reason=?1,prune_after=pg_now_text(CAST(?2 AS TEXT) || ' seconds'),"
       " prune_source_state='',last_progress_at=pg_now_text()"
       " WHERE id=?3 AND state='pruning'";
   char err[SG_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", reason ? reason : "generation prune failed");
   aimee_pg_bind_int(st, "?2", retry_seconds);
   aimee_pg_bind_int64(st, "?3", generation_id);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE)
      rc = aimee_pg_stmt_changes(st) > 0 ? 1 : 0;
   aimee_pg_finalize(st);
   return rc;
}
