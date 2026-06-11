/* db1/roundtable_pipeline.c: durable ledger for the agent roundtable authoring
 * pipeline. See roundtable_pipeline.h and the proposal's section 4. */

#include "roundtable_pipeline.h"
#include "db1_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------- runs ---- */

int rtp_run_create(const char *idea, const char *done_bar, const char *repo_root,
                   const char *base_branch, int *out_id)
{
   if (!idea || !idea[0])
      return -1;

   sqlite3 *db = db1_conn();
   if (!db)
      return -1;

   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO roundtable_pipeline_runs (idea, state, phase, admission_class, done_bar,"
       " repo_root, base_branch, created_at, updated_at)"
       " VALUES (?, 'drafting', 'proposal', 'active', ?, ?, ?, datetime('now'), datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;

   sqlite3_bind_text(stmt, 1, idea, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 2, (done_bar && done_bar[0]) ? done_bar : RTP_DONEBAR_ZERO_BLOCKING, -1,
                     SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, repo_root ? repo_root : "", -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 4, (base_branch && base_branch[0]) ? base_branch : "testing", -1,
                     SQLITE_TRANSIENT);

   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE && out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

static void rtp_map_run(rtp_run_t *r, sqlite3_stmt *s)
{
   memset(r, 0, sizeof(*r));
   r->id = sqlite3_column_int(s, 0);
   db1_copy_col_text(r->idea, sizeof(r->idea), s, 1);
   db1_copy_col_text(r->state, sizeof(r->state), s, 2);
   db1_copy_col_text(r->phase, sizeof(r->phase), s, 3);
   db1_copy_col_text(r->admission_class, sizeof(r->admission_class), s, 4);
   r->schema_version = sqlite3_column_int(s, 5);
   db1_copy_col_text(r->done_bar, sizeof(r->done_bar), s, 6);
   db1_copy_col_text(r->brief, sizeof(r->brief), s, 7);
   db1_copy_col_text(r->gate_digest, sizeof(r->gate_digest), s, 8);
   db1_copy_col_text(r->proposal_ref, sizeof(r->proposal_ref), s, 9);
   db1_copy_col_text(r->proposal_origin_hash, sizeof(r->proposal_origin_hash), s, 10);
   db1_copy_col_text(r->diff_ref, sizeof(r->diff_ref), s, 11);
   db1_copy_col_text(r->diff_origin_hash, sizeof(r->diff_origin_hash), s, 12);
   db1_copy_col_text(r->chunk_index_ref, sizeof(r->chunk_index_ref), s, 13);
   db1_copy_col_text(r->repo_root, sizeof(r->repo_root), s, 14);
   db1_copy_col_text(r->remote, sizeof(r->remote), s, 15);
   db1_copy_col_text(r->base_branch, sizeof(r->base_branch), s, 16);
   db1_copy_col_text(r->head_branch, sizeof(r->head_branch), s, 17);
   db1_copy_col_text(r->workspace_id, sizeof(r->workspace_id), s, 18);
   db1_copy_col_text(r->workspace_provider, sizeof(r->workspace_provider), s, 19);
   db1_copy_col_text(r->worktree_path, sizeof(r->worktree_path), s, 20);
   db1_copy_col_text(r->head_sha, sizeof(r->head_sha), s, 21);
   db1_copy_col_text(r->base_sha, sizeof(r->base_sha), s, 22);
   r->proposal_pr_number = sqlite3_column_int(s, 23);
   db1_copy_col_text(r->proposal_pr_url, sizeof(r->proposal_pr_url), s, 24);
   r->impl_pr_number = sqlite3_column_int(s, 25);
   db1_copy_col_text(r->impl_pr_url, sizeof(r->impl_pr_url), s, 26);
   db1_copy_col_text(r->cost_scope, sizeof(r->cost_scope), s, 27);
   db1_copy_col_text(r->cost_source, sizeof(r->cost_source), s, 28);
   r->cost_version = sqlite3_column_int(s, 29);
   r->proposal_phase_cost_usd = sqlite3_column_double(s, 30);
   r->impl_phase_cost_usd = sqlite3_column_double(s, 31);
   r->total_cost_usd = sqlite3_column_double(s, 32);
   db1_copy_col_text(r->created_at, sizeof(r->created_at), s, 33);
   db1_copy_col_text(r->updated_at, sizeof(r->updated_at), s, 34);
}

#define RTP_RUN_COLS                                                                               \
   "id, idea, state, phase, admission_class, schema_version, done_bar, brief, gate_digest,"        \
   " proposal_ref, proposal_origin_hash, diff_ref, diff_origin_hash, chunk_index_ref, repo_root,"  \
   " remote, base_branch, head_branch, workspace_id, workspace_provider, worktree_path, head_sha," \
   " base_sha, proposal_pr_number, proposal_pr_url, impl_pr_number, impl_pr_url, cost_scope,"      \
   " cost_source, cost_version, proposal_phase_cost_usd, impl_phase_cost_usd, total_cost_usd,"     \
   " created_at, updated_at"

int rtp_run_get(int id, rtp_run_t *out)
{
   if (!out || id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT " RTP_RUN_COLS " FROM roundtable_pipeline_runs WHERE id = ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_map_run(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_run_update(const rtp_run_t *r)
{
   if (!r || r->id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE roundtable_pipeline_runs SET idea=?, state=?, phase=?, admission_class=?,"
       " schema_version=?, done_bar=?, brief=?, gate_digest=?, proposal_ref=?,"
       " proposal_origin_hash=?, diff_ref=?, diff_origin_hash=?, chunk_index_ref=?, repo_root=?,"
       " remote=?, base_branch=?, head_branch=?, workspace_id=?, workspace_provider=?,"
       " worktree_path=?, head_sha=?, base_sha=?, proposal_pr_number=?, proposal_pr_url=?,"
       " impl_pr_number=?, impl_pr_url=?, cost_scope=?, cost_source=?, cost_version=?,"
       " proposal_phase_cost_usd=?, impl_phase_cost_usd=?, total_cost_usd=?,"
       " updated_at=datetime('now') WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   sqlite3_bind_text(stmt, i++, r->idea, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->state, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->phase, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->admission_class, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, r->schema_version ? r->schema_version : 1);
   sqlite3_bind_text(stmt, i++, r->done_bar, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->brief, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->gate_digest, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->proposal_ref, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->proposal_origin_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->diff_ref, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->diff_origin_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->chunk_index_ref, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->repo_root, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->remote, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->base_branch, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->head_branch, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->workspace_id, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->workspace_provider, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->worktree_path, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->head_sha, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->base_sha, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, r->proposal_pr_number);
   sqlite3_bind_text(stmt, i++, r->proposal_pr_url, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, r->impl_pr_number);
   sqlite3_bind_text(stmt, i++, r->impl_pr_url, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->cost_scope, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, r->cost_source, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, r->cost_version ? r->cost_version : 1);
   sqlite3_bind_double(stmt, i++, r->proposal_phase_cost_usd);
   sqlite3_bind_double(stmt, i++, r->impl_phase_cost_usd);
   sqlite3_bind_double(stmt, i++, r->total_cost_usd);
   sqlite3_bind_int(stmt, i++, r->id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int rtp_run_set_state(int id, const char *state, const char *phase)
{
   if (id <= 0 || !state)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   /* phase is optional: NULL keeps the existing value. */
   const char *sql =
       phase ? "UPDATE roundtable_pipeline_runs SET state=?, phase=?, updated_at=datetime('now')"
               " WHERE id=?"
             : "UPDATE roundtable_pipeline_runs SET state=?, updated_at=datetime('now') WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   sqlite3_bind_text(stmt, i++, state, -1, SQLITE_TRANSIENT);
   if (phase)
      sqlite3_bind_text(stmt, i++, phase, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int rtp_run_list(const char *state_filter, rtp_run_t *out, int max)
{
   if (!out || max <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   const char *sql =
       state_filter
           ? "SELECT " RTP_RUN_COLS
             " FROM roundtable_pipeline_runs WHERE state=? ORDER BY updated_at DESC LIMIT ?"
           : "SELECT " RTP_RUN_COLS
             " FROM roundtable_pipeline_runs WHERE state NOT IN ('done','failed','abandoned')"
             " ORDER BY updated_at DESC LIMIT ?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   if (state_filter)
      sqlite3_bind_text(stmt, i++, state_filter, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, max);
   int n = 0;
   while (n < max && sqlite3_step(stmt) == SQLITE_ROW)
      rtp_map_run(&out[n++], stmt);
   sqlite3_finalize(stmt);
   return n;
}

int rtp_run_count_active(void)
{
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COUNT(*) FROM roundtable_pipeline_runs WHERE admission_class='active'"
       " AND state NOT IN ('done','failed','abandoned')";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

/* -------------------------------------------------------------- passes ---- */

int rtp_pass_create(int pipeline_id, const char *phase, const char *mode, int pass_no,
                    const char *artifact_hash, int *out_id)
{
   if (pipeline_id <= 0 || !phase || !mode || pass_no <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO roundtable_pipeline_passes (pipeline_id, phase, mode, pass_no, status,"
       " artifact_hash, created_at, updated_at)"
       " VALUES (?, ?, ?, ?, 'open', ?, datetime('now'), datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_text(stmt, 2, phase, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, 3, mode, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 4, pass_no);
   sqlite3_bind_text(stmt, 5, artifact_hash ? artifact_hash : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE && out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

static void rtp_map_pass(rtp_pass_t *p, sqlite3_stmt *s)
{
   memset(p, 0, sizeof(*p));
   p->id = sqlite3_column_int(s, 0);
   p->pipeline_id = sqlite3_column_int(s, 1);
   db1_copy_col_text(p->phase, sizeof(p->phase), s, 2);
   db1_copy_col_text(p->mode, sizeof(p->mode), s, 3);
   p->pass_no = sqlite3_column_int(s, 4);
   db1_copy_col_text(p->status, sizeof(p->status), s, 5);
   db1_copy_col_text(p->artifact_hash, sizeof(p->artifact_hash), s, 6);
   p->converged = sqlite3_column_int(s, 7);
   p->envelope_valid = sqlite3_column_int(s, 8);
   p->blocking_count = sqlite3_column_int(s, 9);
   p->suggestion_count = sqlite3_column_int(s, 10);
   p->nit_count = sqlite3_column_int(s, 11);
   p->open_questions = sqlite3_column_int(s, 12);
   p->coverage_gaps = sqlite3_column_int(s, 13);
   p->items_round = sqlite3_column_int(s, 14);
   p->artifact_round = sqlite3_column_int(s, 15);
   p->best_round = sqlite3_column_int(s, 16);
   p->rounds_run = sqlite3_column_int(s, 17);
   p->cost_usd = sqlite3_column_double(s, 18);
   db1_copy_col_text(p->result_hash, sizeof(p->result_hash), s, 19);
   p->is_chunked = sqlite3_column_int(s, 20);
   p->chunk_total = sqlite3_column_int(s, 21);
   p->chunk_done = sqlite3_column_int(s, 22);
   p->synthesis_done = sqlite3_column_int(s, 23);
   p->chunk_group = sqlite3_column_int(s, 24);
   p->chunk_index = sqlite3_column_int(s, 25);
   db1_copy_col_text(p->created_at, sizeof(p->created_at), s, 26);
   db1_copy_col_text(p->updated_at, sizeof(p->updated_at), s, 27);
}

#define RTP_PASS_COLS                                                                              \
   "id, pipeline_id, phase, mode, pass_no, status, artifact_hash, converged, envelope_valid,"      \
   " blocking_count, suggestion_count, nit_count, open_questions, coverage_gaps, items_round,"     \
   " artifact_round, best_round, rounds_run, cost_usd, result_hash, is_chunked, chunk_total,"      \
   " chunk_done, synthesis_done, chunk_group, chunk_index, created_at, updated_at"

int rtp_pass_get(int id, rtp_pass_t *out)
{
   if (!out || id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql = "SELECT " RTP_PASS_COLS " FROM roundtable_pipeline_passes WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_map_pass(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_pass_update(const rtp_pass_t *p)
{
   if (!p || p->id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE roundtable_pipeline_passes SET status=?, artifact_hash=?, converged=?,"
       " envelope_valid=?, blocking_count=?, suggestion_count=?, nit_count=?, open_questions=?,"
       " coverage_gaps=?, items_round=?, artifact_round=?, best_round=?, rounds_run=?, cost_usd=?,"
       " result_hash=?, is_chunked=?, chunk_total=?, chunk_done=?, synthesis_done=?,"
       " chunk_group=?, chunk_index=?, updated_at=datetime('now') WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   sqlite3_bind_text(stmt, i++, p->status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, p->artifact_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, p->converged);
   sqlite3_bind_int(stmt, i++, p->envelope_valid);
   sqlite3_bind_int(stmt, i++, p->blocking_count);
   sqlite3_bind_int(stmt, i++, p->suggestion_count);
   sqlite3_bind_int(stmt, i++, p->nit_count);
   sqlite3_bind_int(stmt, i++, p->open_questions);
   sqlite3_bind_int(stmt, i++, p->coverage_gaps);
   sqlite3_bind_int(stmt, i++, p->items_round);
   sqlite3_bind_int(stmt, i++, p->artifact_round);
   sqlite3_bind_int(stmt, i++, p->best_round);
   sqlite3_bind_int(stmt, i++, p->rounds_run);
   sqlite3_bind_double(stmt, i++, p->cost_usd);
   sqlite3_bind_text(stmt, i++, p->result_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, p->is_chunked);
   sqlite3_bind_int(stmt, i++, p->chunk_total);
   sqlite3_bind_int(stmt, i++, p->chunk_done);
   sqlite3_bind_int(stmt, i++, p->synthesis_done);
   sqlite3_bind_int(stmt, i++, p->chunk_group);
   sqlite3_bind_int(stmt, i++, p->chunk_index);
   sqlite3_bind_int(stmt, i++, p->id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int rtp_pass_latest(int pipeline_id, const char *phase, rtp_pass_t *out)
{
   if (!out || pipeline_id <= 0 || !phase)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT " RTP_PASS_COLS " FROM roundtable_pipeline_passes WHERE pipeline_id=? AND phase=?"
       " ORDER BY pass_no DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_text(stmt, 2, phase, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_map_pass(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_pass_max_no(int pipeline_id, const char *phase)
{
   if (pipeline_id <= 0 || !phase)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COALESCE(MAX(pass_no),0) FROM roundtable_pipeline_passes WHERE pipeline_id=?"
       " AND phase=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_text(stmt, 2, phase, -1, SQLITE_TRANSIENT);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int rtp_pass_max_group(int pipeline_id, const char *phase)
{
   if (pipeline_id <= 0 || !phase)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COALESCE(MAX(chunk_group),0) FROM roundtable_pipeline_passes WHERE pipeline_id=?"
       " AND phase=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_text(stmt, 2, phase, -1, SQLITE_TRANSIENT);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int rtp_pass_group_agg(int pipeline_id, const char *phase, int chunk_group, rtp_group_agg_t *out)
{
   if (!out || pipeline_id <= 0 || !phase || chunk_group <= 0)
      return -1;
   memset(out, 0, sizeof(*out));
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT chunk_index, status, envelope_valid, blocking_count, suggestion_count"
       " FROM roundtable_pipeline_passes WHERE pipeline_id=? AND phase=? AND chunk_group=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_text(stmt, 2, phase, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, 3, chunk_group);
   while (sqlite3_step(stmt) == SQLITE_ROW)
   {
      int idx = sqlite3_column_int(stmt, 0);
      const unsigned char *st = sqlite3_column_text(stmt, 1);
      int valid = sqlite3_column_int(stmt, 2);
      int blocking = sqlite3_column_int(stmt, 3);
      int sugg = sqlite3_column_int(stmt, 4);
      int captured = st && (strcmp((const char *)st, "captured") == 0 ||
                            strcmp((const char *)st, "done") == 0);
      out->blocking_count += blocking;
      out->suggestion_count += sugg;
      if (idx < 0) /* the synthesis member */
      {
         out->synthesis_present = 1;
         if (captured && valid)
            out->synthesis_done = 1;
         else if (captured && !valid)
            out->any_invalid = 1;
         continue;
      }
      out->total++;
      if (captured && valid)
         out->done++;
      else if (captured && !valid)
         out->any_invalid = 1;
   }
   sqlite3_finalize(stmt);
   return 0;
}

/* ------------------------------------------------------------ attempts ---- */

int rtp_attempt_create(int pass_id, int attempt_no, const char *run_id, int *out_id)
{
   if (pass_id <= 0 || attempt_no <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO roundtable_pipeline_attempts (pass_id, attempt_no, run_id, is_current,"
       " capture_status, submitted_at) VALUES (?, ?, ?, 1, 'pending', datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pass_id);
   sqlite3_bind_int(stmt, 2, attempt_no);
   sqlite3_bind_text(stmt, 3, run_id ? run_id : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE && out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

static void rtp_map_attempt(rtp_attempt_t *a, sqlite3_stmt *s)
{
   memset(a, 0, sizeof(*a));
   a->id = sqlite3_column_int(s, 0);
   a->pass_id = sqlite3_column_int(s, 1);
   a->attempt_no = sqlite3_column_int(s, 2);
   db1_copy_col_text(a->run_id, sizeof(a->run_id), s, 3);
   a->is_current = sqlite3_column_int(s, 4);
   db1_copy_col_text(a->capture_status, sizeof(a->capture_status), s, 5);
   db1_copy_col_text(a->terminal_status, sizeof(a->terminal_status), s, 6);
   db1_copy_col_text(a->parse_status, sizeof(a->parse_status), s, 7);
   a->envelope_valid = sqlite3_column_int(s, 8);
   a->items_truncated = sqlite3_column_int(s, 9);
   a->truncated = sqlite3_column_int(s, 10);
   a->degraded = sqlite3_column_int(s, 11);
   a->cost_capped = sqlite3_column_int(s, 12);
   a->deadline_hit = sqlite3_column_int(s, 13);
   a->cancelled = sqlite3_column_int(s, 14);
   a->lost_result = sqlite3_column_int(s, 15);
   db1_copy_col_text(a->result_hash, sizeof(a->result_hash), s, 16);
   db1_copy_col_text(a->result_snapshot, sizeof(a->result_snapshot), s, 17);
   a->cost_usd = sqlite3_column_double(s, 18);
   a->cost_known = sqlite3_column_int(s, 19);
   db1_copy_col_text(a->submitted_at, sizeof(a->submitted_at), s, 20);
   db1_copy_col_text(a->terminal_at, sizeof(a->terminal_at), s, 21);
}

#define RTP_ATTEMPT_COLS                                                                           \
   "id, pass_id, attempt_no, run_id, is_current, capture_status, terminal_status, parse_status,"   \
   " envelope_valid, items_truncated, truncated, degraded, cost_capped, deadline_hit, cancelled,"  \
   " lost_result, result_hash, result_snapshot, cost_usd, cost_known, submitted_at, terminal_at"

int rtp_attempt_get_by_run(const char *run_id, rtp_attempt_t *out)
{
   if (!out || !run_id || !run_id[0])
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT " RTP_ATTEMPT_COLS " FROM roundtable_pipeline_attempts WHERE run_id=?"
       " ORDER BY id DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_text(stmt, 1, run_id, -1, SQLITE_TRANSIENT);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_map_attempt(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_attempt_current(int pass_id, rtp_attempt_t *out)
{
   if (!out || pass_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT " RTP_ATTEMPT_COLS " FROM roundtable_pipeline_attempts WHERE pass_id=?"
       " AND is_current=1 ORDER BY attempt_no DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pass_id);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_map_attempt(out, stmt);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_attempt_update(const rtp_attempt_t *a)
{
   if (!a || a->id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE roundtable_pipeline_attempts SET is_current=?, capture_status=?, terminal_status=?,"
       " parse_status=?, envelope_valid=?, items_truncated=?, truncated=?, degraded=?,"
       " cost_capped=?, deadline_hit=?, cancelled=?, lost_result=?, result_hash=?,"
       " result_snapshot=?, cost_usd=?, cost_known=?, terminal_at=? WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   sqlite3_bind_int(stmt, i++, a->is_current);
   sqlite3_bind_text(stmt, i++, a->capture_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, a->terminal_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, a->parse_status, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, a->envelope_valid);
   sqlite3_bind_int(stmt, i++, a->items_truncated);
   sqlite3_bind_int(stmt, i++, a->truncated);
   sqlite3_bind_int(stmt, i++, a->degraded);
   sqlite3_bind_int(stmt, i++, a->cost_capped);
   sqlite3_bind_int(stmt, i++, a->deadline_hit);
   sqlite3_bind_int(stmt, i++, a->cancelled);
   sqlite3_bind_int(stmt, i++, a->lost_result);
   sqlite3_bind_text(stmt, i++, a->result_hash, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, a->result_snapshot, -1, SQLITE_TRANSIENT);
   sqlite3_bind_double(stmt, i++, a->cost_usd);
   sqlite3_bind_int(stmt, i++, a->cost_known);
   sqlite3_bind_text(stmt, i++, (a->terminal_at[0] ? a->terminal_at : ""), -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, a->id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}

int rtp_attempt_max_no(int pass_id)
{
   if (pass_id <= 0)
      return 0;
   sqlite3 *db = db1_conn();
   if (!db)
      return 0;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT COALESCE(MAX(attempt_no),0) FROM roundtable_pipeline_attempts WHERE pass_id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return 0;
   sqlite3_bind_int(stmt, 1, pass_id);
   int n = 0;
   if (sqlite3_step(stmt) == SQLITE_ROW)
      n = sqlite3_column_int(stmt, 0);
   sqlite3_finalize(stmt);
   return n;
}

int rtp_attempt_supersede_others(int pass_id, int keep_attempt_id)
{
   if (pass_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE roundtable_pipeline_attempts SET is_current=0 WHERE pass_id=? AND id<>?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pass_id);
   sqlite3_bind_int(stmt, 2, keep_attempt_id);
   int rc = sqlite3_step(stmt);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

/* --------------------------------------------------------------- gates ---- */

int rtp_gate_create(int pipeline_id, int gate_no, int pr_number, const char *expected_head_sha,
                    int *out_id)
{
   if (pipeline_id <= 0 || (gate_no != 1 && gate_no != 2))
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "INSERT INTO roundtable_pipeline_gates (pipeline_id, gate_no, pr_number, expected_head_sha,"
       " created_at) VALUES (?, ?, ?, ?, datetime('now'))";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_int(stmt, 2, gate_no);
   sqlite3_bind_int(stmt, 3, pr_number);
   sqlite3_bind_text(stmt, 4, expected_head_sha ? expected_head_sha : "", -1, SQLITE_TRANSIENT);
   int rc = sqlite3_step(stmt);
   if (rc == SQLITE_DONE && out_id)
      *out_id = (int)sqlite3_last_insert_rowid(db);
   sqlite3_finalize(stmt);
   return rc == SQLITE_DONE ? 0 : -1;
}

int rtp_gate_get(int pipeline_id, int gate_no, rtp_gate_t *out)
{
   if (!out || pipeline_id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "SELECT id, pipeline_id, gate_no, verdict, reason, actor, pr_number, expected_head_sha,"
       " merge_sha, merge_executor, merge_command, merge_output, merge_exit_code, resolved_at,"
       " created_at FROM roundtable_pipeline_gates WHERE pipeline_id=? AND gate_no=?"
       " ORDER BY id DESC LIMIT 1";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   sqlite3_bind_int(stmt, 1, pipeline_id);
   sqlite3_bind_int(stmt, 2, gate_no);
   int rc = -1;
   if (sqlite3_step(stmt) == SQLITE_ROW)
   {
      rtp_gate_t *g = out;
      memset(g, 0, sizeof(*g));
      g->id = sqlite3_column_int(stmt, 0);
      g->pipeline_id = sqlite3_column_int(stmt, 1);
      g->gate_no = sqlite3_column_int(stmt, 2);
      db1_copy_col_text(g->verdict, sizeof(g->verdict), stmt, 3);
      db1_copy_col_text(g->reason, sizeof(g->reason), stmt, 4);
      db1_copy_col_text(g->actor, sizeof(g->actor), stmt, 5);
      g->pr_number = sqlite3_column_int(stmt, 6);
      db1_copy_col_text(g->expected_head_sha, sizeof(g->expected_head_sha), stmt, 7);
      db1_copy_col_text(g->merge_sha, sizeof(g->merge_sha), stmt, 8);
      db1_copy_col_text(g->merge_executor, sizeof(g->merge_executor), stmt, 9);
      db1_copy_col_text(g->merge_command, sizeof(g->merge_command), stmt, 10);
      db1_copy_col_text(g->merge_output, sizeof(g->merge_output), stmt, 11);
      g->merge_exit_code = sqlite3_column_int(stmt, 12);
      db1_copy_col_text(g->resolved_at, sizeof(g->resolved_at), stmt, 13);
      db1_copy_col_text(g->created_at, sizeof(g->created_at), stmt, 14);
      rc = 0;
   }
   sqlite3_finalize(stmt);
   return rc;
}

int rtp_gate_update(const rtp_gate_t *g)
{
   if (!g || g->id <= 0)
      return -1;
   sqlite3 *db = db1_conn();
   if (!db)
      return -1;
   sqlite3_stmt *stmt = NULL;
   static const char *sql =
       "UPDATE roundtable_pipeline_gates SET verdict=?, reason=?, actor=?, pr_number=?,"
       " expected_head_sha=?, merge_sha=?, merge_executor=?, merge_command=?, merge_output=?,"
       " merge_exit_code=?, resolved_at=? WHERE id=?";
   if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
      return -1;
   int i = 1;
   sqlite3_bind_text(stmt, i++, g->verdict, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->reason, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->actor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, g->pr_number);
   sqlite3_bind_text(stmt, i++, g->expected_head_sha, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->merge_sha, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->merge_executor, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->merge_command, -1, SQLITE_TRANSIENT);
   sqlite3_bind_text(stmt, i++, g->merge_output, -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, g->merge_exit_code);
   sqlite3_bind_text(stmt, i++, (g->resolved_at[0] ? g->resolved_at : ""), -1, SQLITE_TRANSIENT);
   sqlite3_bind_int(stmt, i++, g->id);
   int rc = sqlite3_step(stmt);
   int changed = (rc == SQLITE_DONE) ? sqlite3_changes(db) : 0;
   sqlite3_finalize(stmt);
   return changed > 0 ? 0 : -1;
}
