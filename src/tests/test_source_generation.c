/* test_source_generation.c: immutable snapshot + atomic moving-ref publication. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../db2/db2_internal.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db_postgres.h"
#include "../db2/source_generation.h"

static void exec_ok(const char *sql)
{
   char err[256] = "";
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
}

static void insert_file(int64_t project_id, const char *path)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO files(project_id,path,hash,scanned_at)"
       " VALUES(?1,?2,'filehash',pg_now_text())",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_int64(st, "?1", project_id);
   aimee_pg_bind_text(st, "?2", path);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_DONE);
   aimee_pg_finalize(st);
}

static int64_t make_original(const char *project, const char *path, char sha_char)
{
   char sha[65];
   memset(sha, sha_char, 64);
   sha[64] = '\0';
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO kb_original_documents(project,source_uri,source_kind,created_at)"
       " VALUES(?1,?2,'git_blob',pg_now_text()) RETURNING id",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", path);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t document_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   st = aimee_pg_prepare(
       db2_conn(),
       "INSERT INTO kb_original_versions"
       " (document_id,original_sha256,original_byte_length,media_type,original_blob_ref,"
       "  source_revision,captured_at) VALUES(?1,?2,7,'text/plain',?2,'commit',pg_now_text())"
       " RETURNING id",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_int64(st, "?1", document_id);
   aimee_pg_bind_text(st, "?2", sha);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int64_t version_id = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return version_id;
}

static int scalar_int(const char *sql, int64_t arg)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   aimee_pg_bind_int64(st, "?1", arg);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   int value = aimee_pg_column_int(st, 0);
   aimee_pg_finalize(st);
   return value;
}

static void assert_generation_state(int64_t generation_id, const char *expected)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT state FROM kb_index_generations WHERE id=?1", err, sizeof(err));
   assert(st);
   aimee_pg_bind_int64(st, "?1", generation_id);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(strcmp(aimee_pg_column_text(st, 0), expected) == 0);
   aimee_pg_finalize(st);
}

static void assert_ref_lifecycle(const char *ref_name, const char *expected)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT lifecycle_state FROM kb_source_refs WHERE ref_name=?1",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", ref_name);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   assert(strcmp(aimee_pg_column_text(st, 0), expected) == 0);
   aimee_pg_finalize(st);
}

int main(void)
{
   static const char *c1 = "1111111111111111111111111111111111111111";
   static const char *t1 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
   static const char *c2 = "2222222222222222222222222222222222222222";
   static const char *t2 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
   static const char *c3 = "3333333333333333333333333333333333333333";
   static const char *t3 = "cccccccccccccccccccccccccccccccccccccccc";
   db2_source_generation_t g1, resumed, resolved;

   db2_test_shim_open();

   assert(db2_source_generation_begin("github:org/repo", "refs/heads/main", 1, c1, t1,
                                      "remote", &g1) == 0);
   assert(g1.generation_id > 0 && g1.project_id > 0);
   assert(strcmp(g1.state, "staging") == 0);
   assert(strncmp(g1.physical_project, "generation:", 11) == 0);

   assert(db2_source_generation_begin("github:org/repo", "refs/heads/main", 1, c1, t1,
                                      "remote", &resumed) == 0);
   assert(resumed.generation_id == g1.generation_id);
   assert(resumed.reused_snapshot == 1);
   assert(resumed.already_current == 0); /* incomplete data is never current */

   insert_file(g1.project_id, "src/a.c");
   insert_file(g1.project_id, "src/b.c");
   int64_t original_a = make_original(g1.physical_project, "src/a.c", 'd');
   int64_t original_b = make_original(g1.physical_project, "src/b.c", 'e');
   assert(db2_source_generation_link_file_evidence(g1.generation_id, "src/a.c", original_a) ==
          1);
   assert(db2_source_generation_link_file_evidence(g1.generation_id, "src/b.c", original_b) ==
          1);
   assert(scalar_int("SELECT COUNT(*) FROM kb_evidence_lineage"
                     " WHERE evidence_tier='secondary' AND subject_kind='code_file'"
                     " AND index_generation_id=?1",
                     g1.generation_id) == 2);
   assert(db2_source_generation_source_complete(g1.generation_id, t1, 3, &resumed) == -1);
   assert_generation_state(g1.generation_id, "staging");
   assert(db2_source_generation_source_complete(g1.generation_id, t1, 2, &resumed) == 0);
   assert(strcmp(resumed.state, "encoding") == 0);
   assert(db2_source_generation_model_complete(g1.generation_id, 2, 1, "model", "cp", "tok",
                                               &resumed) == -1);
   exec_ok("INSERT INTO code_embeddings"
           " (point_id,project,node_key,file_path,content_hash,source_hash)"
           " SELECT f.id,p.name,'file:' || f.path,f.path,f.hash,f.hash FROM files f"
           " JOIN projects p ON p.id=f.project_id WHERE p.name LIKE 'generation:%'");
   assert(db2_source_generation_record_embedding_lineage(g1.generation_id, "model") == 2);
   assert(scalar_int("SELECT COUNT(*) FROM kb_evidence_lineage"
                     " WHERE evidence_tier='tertiary' AND subject_kind='code_embedding'"
                     " AND parent_evidence_kind='code_file' AND index_generation_id=?1",
                     g1.generation_id) == 2);
   assert(db2_source_generation_model_complete(g1.generation_id, 2, 2, "model", "cp", "tok",
                                               &resumed) == 0);
   assert(strcmp(resumed.state, "validating") == 0);
   assert(db2_source_generation_publish(g1.generation_id, &resumed) == 0);
   assert(strcmp(resumed.state, "published") == 0);

   assert(db2_source_ref_resolve_current("github:org/repo", "", &resolved) == 1);
   assert(resolved.generation_id == g1.generation_id);
   assert(strcmp(resolved.source_ref, "refs/heads/main") == 0);
   assert(resolved.is_default == 1);

   /* A second ref at identical bytes reuses the encoded generation. */
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/feature", 0, c1, t1,
                                      "remote", &resumed) == 0);
   assert(resumed.generation_id == g1.generation_id);
   assert(resumed.reused_snapshot == 1 && resumed.already_current == 0);
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/feature", &resolved) ==
          1);
   assert(resolved.generation_id == g1.generation_id);
   assert(strcmp(resolved.source_ref, "refs/heads/feature") == 0);
   assert(resolved.is_default == 0);

   /* A merged ref stops resolving immediately, but a shared generation cannot
    * be collected while main still points to it. An explicit request to index
    * the ref again reactivates its tombstone. */
   assert(db2_source_ref_retire("github:org/repo", "refs/heads/feature", "merged", 0) == 1);
   assert_ref_lifecycle("refs/heads/feature", "merged");
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/feature", &resolved) ==
          0);
   db2_source_prune_candidate_t prune;
   assert(db2_source_generation_prune_claim(&prune) == 0);
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/feature", 0, c1, t1,
                                      "remote", &resumed) == 0);
   assert_ref_lifecycle("refs/heads/feature", "active");
   assert(resumed.generation_id == g1.generation_id && resumed.already_current == 0);

   /* Moving main starts an isolated generation and makes stale main fail closed;
    * feature continues to resolve its still-current old snapshot. */
   db2_source_generation_t g2;
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/main", 1, c2, t2,
                                      "remote", &g2) == 0);
   assert(g2.generation_id != g1.generation_id && g2.project_id != g1.project_id);
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/main", &resolved) == 0);
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/feature", &resolved) ==
          1);
   assert(resolved.generation_id == g1.generation_id);
   insert_file(g2.project_id, "src/new.c");
   assert(db2_source_generation_source_complete(g2.generation_id, t2, 1, &resumed) == 0);
   assert(db2_source_generation_model_complete(g2.generation_id, 1, 1, "model", "cp", "tok",
                                               &resumed) == 0);

   /* Ref movement during encoding cannot publish the wrong branch snapshot. */
   db2_source_generation_t moved;
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/main", 1, c3, t3,
                                      "remote", &moved) == 0);
   assert(db2_source_generation_publish(g2.generation_id, &resumed) == -2);
   assert_generation_state(g2.generation_id, "aborted");

   /* Abort is recoverable: a fresh physical generation can retry the same
    * snapshot without colliding with the abandoned attempt. */
   assert(db2_source_generation_abort(moved.generation_id, "worker interrupted") == 0);
   db2_source_generation_t retry;
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/main", 1, c3, t3,
                                      "remote", &retry) == 0);
   assert(retry.generation_id != moved.generation_id);
   assert(retry.project_id != moved.project_id);
   insert_file(retry.project_id, "src/latest.c");
   assert(db2_source_generation_source_complete(retry.generation_id, t3, 1, &resumed) == 0);
   assert(db2_source_generation_model_complete(retry.generation_id, 1, 1, "model", "cp", "tok",
                                               &resumed) == 0);
   assert(db2_source_generation_publish(retry.generation_id, &resumed) == 0);
   assert(db2_source_ref_resolve_current("github:org/repo", "", &resolved) == 1);
   assert(resolved.generation_id == retry.generation_id);
   assert(strcmp(resolved.commit_sha, c3) == 0);

   /* Once feature advances to c3 too, the no-longer-referenced c1 generation
    * becomes historical/superseded, but remains reusable if a ref returns. */
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/feature", 0, c3, t3,
                                      "remote", &resumed) == 0);
   assert(resumed.generation_id == retry.generation_id);
   assert_generation_state(g1.generation_id, "superseded");
   assert(db2_source_generation_begin("github:org/repo", "refs/heads/release-old", 0, c1, t1,
                                      "remote", &resumed) == 0);
   assert(resumed.generation_id == g1.generation_id);
   assert_generation_state(g1.generation_id, "published");
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/release-old", &resolved) ==
          1);
   assert(resolved.generation_id == g1.generation_id);

   /* Deleted/merged tombstones detach immediately. Once no active ref shares
    * the old generation, a zero-grace policy makes it claimable. The finalizer
    * removes relational derivations and originals; physical blob bytes remain
    * under content-addressed reconciliation until truly unreferenced. */
   assert(db2_source_ref_retire("github:org/repo", "refs/heads/release-old", "deleted", 0) == 1);
   assert_ref_lifecycle("refs/heads/release-old", "deleted");
   assert(db2_source_ref_resolve_current("github:org/repo", "refs/heads/release-old", &resolved) ==
          0);
   assert(db2_source_generation_prune_claim(&prune) == 1);
   assert(prune.generation_id == g1.generation_id);
   assert(strcmp(prune.prior_state, "superseded") == 0);
   assert(strcmp(prune.physical_project, g1.physical_project) == 0);
   {
      char sql[512];
      snprintf(sql, sizeof(sql), "DELETE FROM code_embeddings WHERE project='%s'",
               g1.physical_project);
      exec_ok(sql); /* stand-in for the service's external-store purge fan-out */
   }
   assert(db2_source_generation_prune_finalize(g1.generation_id) == 1);
   assert(db2_source_generation_get(g1.generation_id, &resolved) == 0);
   assert(scalar_int("SELECT COUNT(*) FROM kb_original_documents WHERE id>?1", 0) == 0);

   exec_ok("PRAGMA foreign_keys=ON");
   db2_test_shim_close();
   puts("source-generation: all tests passed");
   return 0;
}
