/* test_code_project_lifecycle.c: E2 stable identity + audited lifecycle contract. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../db2/code_index.h"
#include "../db2/code_project_lifecycle.h"
#include "../db2/db2_internal.h"
#include "../db2/db2_test_shim.h"
#include "../db2/db_postgres.h"
#include "../db2/kb_audit_worm.h"

static long scalar(const char *sql)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(db2_conn(), sql, err, sizeof(err));
   assert(st);
   assert(aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW);
   long out = (long)aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return out;
}

static long target_rows(const code_project_manifest_t *m, const char *table)
{
   for (int i = 0; i < m->target_count; i++)
      if (strcmp(m->targets[i].table, table) == 0)
         return m->targets[i].rows;
   return -1;
}

static void exec_ok(const char *sql)
{
   char err[256] = "";
   assert(aimee_pg_exec(db2_conn(), sql, err, sizeof(err)) == 0);
}

static void test_move_detach_readd(void)
{
   int64_t first = db2_code_index_project_upsert("stable-project", "/checkout/old");
   assert(first > 0);
   int64_t first_file = db2_code_index_file_upsert(first, "src/main.c", "2026-01-01T00:00:00Z");
   assert(first_file > 0);
   exec_ok("INSERT INTO terms(file_id,name,kind,line)"
           " SELECT id,'stable_symbol','definition',1 FROM files WHERE project_id="
           " (SELECT id FROM projects WHERE name='stable-project')");
   exec_ok("INSERT INTO kb_file_index(project,generation,file_path,file_hash)"
           " VALUES('stable-project',1,'docs/same.md','old')");
   exec_ok("INSERT INTO kb_code_unit_jobs(project,generation,file_path,symbol)"
           " VALUES('stable-project',1,'src/main.c','stable_symbol')");
   exec_ok("INSERT INTO kb_minhash_signatures(project,generation,file_path,signature_bytes)"
           " VALUES('stable-project',1,'docs/same.md',x'01')");
   exec_ok("INSERT INTO kb_lsh_buckets(project,generation,band,band_hash,file_path)"
           " VALUES('stable-project',1,0,'same','docs/same.md')");
   exec_ok("INSERT INTO code_projection_generations(project,state)"
           " VALUES('stable-project','visible')");
   exec_ok("INSERT INTO entity_edges(source,relation,target,edge_origin,projection_generation_id)"
           " SELECT 'project:stable-project','contains','file:stable-project:src/main.c',"
           " 'code_projection',id FROM code_projection_generations"
           " WHERE project='stable-project' AND state='visible'");
   int64_t other = db2_code_index_project_upsert("other-project", "/checkout/other");
   assert(other > 0 && other != first);

   int64_t moved = db2_code_index_project_upsert("stable-project", "/checkout/new");
   assert(moved == first);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='stable-project'") == 1);
   assert(scalar("SELECT current_generation FROM projects WHERE name='stable-project'") == 1);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE project_id="
                 " (SELECT id FROM projects WHERE name='stable-project')") == 2);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE is_current=1 AND project_id="
                 " (SELECT id FROM projects WHERE name='stable-project')") == 1);

   int64_t detached_generation = 0;
   exec_ok("ALTER TABLE kb_audit_event RENAME TO kb_audit_event_unavailable");
   assert(db2_code_project_detach("stable-project", "tester", &detached_generation) ==
          CODE_PROJECT_LIFECYCLE_AUDIT_FAILED);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='stable-project'"
                 " AND lifecycle_state='current'") == 1);
   exec_ok("ALTER TABLE kb_audit_event_unavailable RENAME TO kb_audit_event");
   assert(db2_code_project_detach("stable-project", "tester", &detached_generation) == 0);
   assert(detached_generation == 1);
   assert(scalar("SELECT COUNT(*) FROM code_projection_generations"
                 " WHERE project='stable-project' AND state='visible'") == 0);
   assert(scalar("SELECT COUNT(*) FROM entity_edges WHERE edge_origin='code_projection'"
                 " AND source='project:stable-project'") == 0);
   assert(scalar("SELECT COUNT(*) FROM kb_audit_event WHERE action='code.index.detach'"
                 " AND actor_principal='tester' AND subject='stable-project'") == 1);
   project_info_t projects[8];
   int listed = db2_code_index_project_list(projects, 8);
   assert(listed == 1 && strcmp(projects[0].name, "other-project") == 0);
   term_hit_t hits[8];
   assert(db2_code_index_term_find("stable_symbol", hits, 8) == 0);
   assert(scalar("SELECT COUNT(*) FROM files WHERE project_id="
                 " (SELECT id FROM projects WHERE name='stable-project')") == 1);

   /* Re-adding is a new current generation of the same stable project. */
   assert(db2_code_index_project_upsert("stable-project", "/checkout/new") == first);
   assert(scalar("SELECT current_generation FROM projects WHERE name='stable-project'") == 2);
   assert(db2_code_index_term_find("stable_symbol", hits, 8) == 0);
   int64_t second_file = db2_code_index_file_upsert(first, "src/main.c", "2026-01-02T00:00:00Z");
   assert(second_file > 0 && second_file != first_file);
   exec_ok("INSERT INTO terms(file_id,name,kind,line)"
           " SELECT id,'stable_symbol','definition',2 FROM files WHERE project_id="
           " (SELECT id FROM projects WHERE name='stable-project') AND generation=2");
   exec_ok("INSERT INTO kb_file_index(project,generation,file_path,file_hash)"
           " VALUES('stable-project',2,'docs/same.md','new')");
   exec_ok("INSERT INTO kb_code_unit_jobs(project,generation,file_path,symbol)"
           " VALUES('stable-project',2,'src/main.c','stable_symbol')");
   exec_ok("INSERT INTO kb_minhash_signatures(project,generation,file_path,signature_bytes)"
           " VALUES('stable-project',2,'docs/same.md',x'02')");
   exec_ok("INSERT INTO kb_lsh_buckets(project,generation,band,band_hash,file_path)"
           " VALUES('stable-project',2,0,'same','docs/same.md')");
   assert(scalar("SELECT COUNT(*) FROM kb_file_index WHERE project='stable-project'"
                 " AND file_path='docs/same.md'") == 2);
   assert(scalar("SELECT COUNT(*) FROM kb_code_unit_jobs WHERE project='stable-project'"
                 " AND file_path='src/main.c' AND symbol='stable_symbol'") == 2);
   assert(scalar("SELECT COUNT(*) FROM kb_minhash_signatures WHERE project='stable-project'"
                 " AND file_path='docs/same.md'") == 2);
   assert(scalar("SELECT COUNT(*) FROM kb_lsh_buckets WHERE project='stable-project'"
                 " AND file_path='docs/same.md'") == 2);
   assert(db2_code_index_term_find("stable_symbol", hits, 8) == 1);
   assert(hits[0].line == 2);
   assert(scalar("SELECT COUNT(*) FROM files WHERE project_id="
                 " (SELECT id FROM projects WHERE name='stable-project')") == 2);
   puts("  PASS: move, detach, and re-add retain identity while fencing stale generations");
}

static void test_manifest_confirmation_and_audit(void)
{
   assert(db2_kb_audit_append_in_txn(db2_conn(), "operator", "tester", "misuse", "project", "deny",
                                     "{}") == -1);
   exec_ok("INSERT INTO kb_documents(project,file_path,file_hash,chunk_index,content)"
           " VALUES('stable-project','docs/a.pdf','h',0,'chunk'),"
           " ('stable-project','docs/a.pdf','h',1,'legacy chunk')");
   exec_ok("INSERT INTO vector_index_ops(point_id,collection)"
           " SELECT id,CASE WHEN chunk_index=1 THEN 'kb_chunks' ELSE 'kb_embeddings' END"
           " FROM kb_documents WHERE project='stable-project'");
   exec_ok("INSERT INTO kb_doc_assets(project,document_key,page_no,blob_ref)"
           " VALUES('stable-project','docs/a.pdf',1,'blob-a'),"
           " ('other-project','docs/a.pdf',1,'blob-other')");
   exec_ok("INSERT INTO kb_runtime_state(state_key,state_value)"
           " VALUES('code_scan_sha:stable-project','abc'),('code_scan_sha:other-project','def')");
   exec_ok("INSERT INTO artifacts(id,kind,scope_kind,scope_id)"
           " VALUES('artifact-stable','code_unit','project','stable-project'),"
           " ('artifact-other','code_unit','project','other-project')");
   exec_ok("INSERT INTO curator_code_unit_vectors(point_id,artifact_id)"
           " VALUES(901,'artifact-stable'),(902,'artifact-other')");

   code_project_manifest_t dry;
   assert(db2_code_project_purge_manifest("stable-project", &dry) == 0);
   assert(strcmp(dry.operation, "purge") == 0);
   assert(strcmp(dry.mode, "dry_run") == 0);
   assert(target_rows(&dry, "projects") == 1);
   assert(target_rows(&dry, "files") == 2);
   assert(target_rows(&dry, "terms") == 2);
   assert(target_rows(&dry, "vector_index_ops") == 2);
   assert(target_rows(&dry, "kb_doc_assets") == 1);
   assert(target_rows(&dry, "kb_runtime_state") == 1);
   assert(target_rows(&dry, "curator_code_unit_vectors") == 1);

   /* Projection tables were introduced after the oldest supported databases.
    * A purge can still manifest and delete every available target during the
    * migration window. */
   exec_ok("ALTER TABLE code_projection_edges RENAME TO code_projection_edges_unavailable");
   exec_ok("ALTER TABLE code_projection_communities"
           " RENAME TO code_projection_communities_unavailable");
   exec_ok("ALTER TABLE code_projection_generations"
           " RENAME TO code_projection_generations_unavailable");

   /* The confirmation hash fences the exact target set. */
   exec_ok("INSERT INTO files(project_id,path,scanned_at)"
           " SELECT id,'src/late.c','2026-01-01T00:00:00Z' FROM projects"
           " WHERE name='stable-project'");
   code_project_manifest_t confirmed;
   assert(db2_code_project_purge_confirm("stable-project", dry.manifest_hash, "tester", "cleanup",
                                         &confirmed) == CODE_PROJECT_LIFECYCLE_HASH_MISMATCH);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='stable-project'") == 1);

   assert(db2_code_project_purge_manifest("stable-project", &dry) == 0);
   char unchanged_count_hash[72];
   snprintf(unchanged_count_hash, sizeof(unchanged_count_hash), "%s", dry.manifest_hash);
   exec_ok("DELETE FROM files WHERE project_id=(SELECT id FROM projects"
           " WHERE name='stable-project') AND path='src/late.c'");
   exec_ok("INSERT INTO files(project_id,path,scanned_at)"
           " SELECT id,'src/replacement.c','2026-01-01T00:00:00Z' FROM projects"
           " WHERE name='stable-project'");
   assert(db2_code_project_purge_confirm("stable-project", unchanged_count_hash, "tester",
                                         "cleanup",
                                         &confirmed) == CODE_PROJECT_LIFECYCLE_HASH_MISMATCH);
   assert(scalar("SELECT COUNT(*) FROM files WHERE project_id="
                 " (SELECT id FROM projects WHERE name='stable-project')") == 3);

   assert(db2_code_project_purge_manifest("stable-project", &dry) == 0);
   char hash[72];
   snprintf(hash, sizeof(hash), "%s", dry.manifest_hash);

   /* If the standard WORM audit store cannot accept a row, no deletion lands. */
   exec_ok("ALTER TABLE kb_audit_event RENAME TO kb_audit_event_unavailable");
   assert(db2_code_project_purge_confirm("stable-project", hash, "tester", "cleanup", &confirmed) ==
          CODE_PROJECT_LIFECYCLE_AUDIT_FAILED);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='stable-project'") == 1);
   exec_ok("ALTER TABLE kb_audit_event_unavailable RENAME TO kb_audit_event");

   char escaped_reason[513];
   memset(escaped_reason, '\n', sizeof(escaped_reason) - 1);
   memcpy(escaped_reason, "cleanup", 7);
   escaped_reason[sizeof(escaped_reason) - 1] = '\0';
   assert(db2_code_project_purge_confirm("stable-project", hash, "tester", escaped_reason,
                                         &confirmed) == 0);
   assert(strcmp(confirmed.mode, "confirmed") == 0);
   assert(strcmp(confirmed.manifest_hash, hash) == 0);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='stable-project'") == 0);
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='other-project'") == 1);
   assert(scalar("SELECT COUNT(*) FROM vector_index_ops") == 0);
   assert(scalar("SELECT COUNT(*) FROM kb_doc_assets WHERE project='stable-project'") == 0);
   assert(scalar("SELECT COUNT(*) FROM kb_doc_assets WHERE project='other-project'") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_runtime_state"
                 " WHERE state_key='code_scan_sha:stable-project'") == 0);
   assert(scalar("SELECT COUNT(*) FROM kb_runtime_state"
                 " WHERE state_key='code_scan_sha:other-project'") == 1);
   assert(scalar("SELECT COUNT(*) FROM curator_code_unit_vectors WHERE point_id=901") == 0);
   assert(scalar("SELECT COUNT(*) FROM curator_code_unit_vectors WHERE point_id=902") == 1);
   /* Purge clears index derivatives, not durable curator artifacts/memory. */
   assert(scalar("SELECT COUNT(*) FROM artifacts WHERE id='artifact-stable'") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_audit_event WHERE action='code.index.purge'"
                 " AND actor_principal='tester' AND subject='stable-project'"
                 " AND detail LIKE '%manifest_hash%' AND detail LIKE '%generation%'"
                 " AND detail LIKE '%cleanup\\u000a%' AND length(detail)>3000") == 1);
   exec_ok("ALTER TABLE code_projection_edges_unavailable RENAME TO code_projection_edges");
   exec_ok("ALTER TABLE code_projection_communities_unavailable"
           " RENAME TO code_projection_communities");
   exec_ok("ALTER TABLE code_projection_generations_unavailable"
           " RENAME TO code_projection_generations");
   puts("  PASS: exact dry-run hash, fail-closed audit, and project-only purge");
}

static void test_gc_audit(void)
{
   int64_t id = db2_code_index_project_upsert("gc-project", "/gc/old");
   assert(id > 0);
   int64_t old_file = db2_code_index_file_upsert(id, "src/gc.c", "2026-01-01T00:00:00Z");
   assert(old_file > 0);
   assert(db2_code_index_project_upsert("gc-project", "/gc/new") == id);
   int64_t detached_generation = 0;
   assert(db2_code_project_detach("gc-project", "tester", &detached_generation) == 0);
   assert(detached_generation == 1);
   assert(db2_code_index_project_upsert("gc-project", "/gc/new") == id);
   assert(scalar("SELECT current_generation FROM projects WHERE name='gc-project'") == 2);
   int64_t current_file = db2_code_index_file_upsert(id, "src/gc.c", "2026-01-02T00:00:00Z");
   assert(current_file > 0 && current_file != old_file);
   exec_ok("INSERT INTO code_embeddings(point_id,project,generation,node_key,content_hash)"
           " VALUES(8101,'gc-project',1,'file:gc-project:old.c','old'),"
           " (8102,'gc-project',2,'file:gc-project:new.c','new')");
   exec_ok("INSERT INTO kb_documents(id,project,generation,file_path,file_hash,chunk_index,content)"
           " VALUES(8201,'gc-project',1,'docs/old.md','old',0,'old'),"
           " (8202,'gc-project',2,'docs/new.md','new',0,'new')");
   exec_ok("INSERT INTO kb_doc_assets(id,project,generation,document_key,blob_ref)"
           " VALUES(8301,'gc-project',1,'docs/old.md','old-blob'),"
           " (8302,'gc-project',2,'docs/new.md','new-blob')");
   exec_ok("INSERT INTO kb_file_index(project,generation,file_path,file_hash)"
           " VALUES('gc-project',1,'docs/retired.md','old'),"
           " ('gc-project',2,'docs/current.md','new')");
   exec_ok("INSERT INTO kb_minhash_signatures"
           "(project,generation,file_path,signature_bytes)"
           " VALUES('gc-project',1,'src/old.c',x'01'),('gc-project',2,'src/new.c',x'02')");
   exec_ok("INSERT INTO kb_lsh_buckets(project,generation,band,band_hash,file_path)"
           " VALUES('gc-project',1,0,'old','src/old.c'),"
           " ('gc-project',2,0,'new','src/new.c')");
   exec_ok("INSERT INTO kb_code_unit_jobs(project,generation,file_path,symbol)"
           " VALUES('gc-project',1,'src/gc.c','gc_fn'),"
           " ('gc-project',2,'src/gc.c','gc_fn')");
   exec_ok("INSERT INTO css_migration_units(project,generation,unit_path)"
           " VALUES('gc-project',1,'src/Gc.tsx'),('gc-project',2,'src/Gc.tsx')");
   exec_ok("INSERT INTO css_render_snapshots(project,generation,unit_path,phase,snapshot)"
           " VALUES('gc-project',1,'src/Gc.tsx','before','old'),"
           " ('gc-project',2,'src/Gc.tsx','before','new')");
   exec_ok("UPDATE code_project_aliases SET last_seen_at='2000-01-01T00:00:00Z'"
           " WHERE project_id=(SELECT id FROM projects WHERE name='gc-project') AND is_current=0");
   exec_ok("UPDATE code_project_generations SET detached_at='2000-01-01T00:00:00Z'"
           " WHERE project_id=(SELECT id FROM projects WHERE name='gc-project')"
           " AND state<>'current'");
   code_project_manifest_t dry, done;
   assert(db2_code_project_gc_manifest("gc-project", 30, &dry) == 0);
   assert(strcmp(dry.operation, "gc") == 0);
   assert(dry.total_rows == 12);
   assert(target_rows(&dry, "files.retired") == 1);
   assert(target_rows(&dry, "code_embeddings.retired") == 1);
   assert(target_rows(&dry, "kb_documents.retired") == 1);
   assert(strstr(dry.criteria, "retention_days=30;") == dry.criteria);
   char hash[72];
   snprintf(hash, sizeof(hash), "%s", dry.manifest_hash);

   /* Retention criteria are part of the confirmation token even when two
    * windows currently select the same number of rows. */
   assert(db2_code_project_gc_confirm("gc-project", 60, hash, "tester", "retention", &done) ==
          CODE_PROJECT_LIFECYCLE_HASH_MISMATCH);

   exec_ok("ALTER TABLE kb_audit_event RENAME TO kb_audit_event_unavailable");
   assert(db2_code_project_gc_confirm("gc-project", 30, hash, "tester", "retention", &done) ==
          CODE_PROJECT_LIFECYCLE_AUDIT_FAILED);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE project_id="
                 " (SELECT id FROM projects WHERE name='gc-project')") == 2);
   assert(scalar("SELECT COUNT(*) FROM files WHERE project_id="
                 " (SELECT id FROM projects WHERE name='gc-project')") == 2);
   exec_ok("ALTER TABLE kb_audit_event_unavailable RENAME TO kb_audit_event");

   assert(db2_code_project_gc_confirm("gc-project", 30, hash, "tester", "retention", &done) == 0);
   assert(strcmp(done.mode, "confirmed") == 0);
   assert(strcmp(done.manifest_hash, hash) == 0);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE project_id="
                 " (SELECT id FROM projects WHERE name='gc-project')") == 1);
   assert(scalar("SELECT COUNT(*) FROM code_project_generations WHERE project_id="
                 " (SELECT id FROM projects WHERE name='gc-project')") == 1);
   assert(scalar("SELECT COUNT(*) FROM files WHERE project_id="
                 " (SELECT id FROM projects WHERE name='gc-project') AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM code_embeddings WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_documents WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_doc_assets WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_file_index WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_minhash_signatures WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_lsh_buckets WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_code_unit_jobs WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM css_migration_units WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM css_render_snapshots WHERE project='gc-project'"
                 " AND generation=2") == 1);
   assert(scalar("SELECT COUNT(*) FROM kb_audit_event WHERE action='code.index.gc'"
                 " AND actor_principal='tester' AND subject='gc-project'") == 1);
   puts("  PASS: retention GC is manifest-fenced, audited, and fail-closed");
}

/* A checkout claimed by another project is a RE-INDEX under a new name, not an
 * error. This used to roll the whole upsert back and return -1, which the HTTP
 * route rendered as "canonical index scan failed" with nothing logged -- and it
 * was permanent, so a caller that mints a fresh project name per attempt could
 * scan a directory exactly once, ever. */
static void test_reindex_under_new_name_takes_the_alias(void)
{
   const char *root = "/checkout/shared";
   int64_t owner = db2_code_index_project_upsert("first-owner", root);
   assert(owner > 0);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE alias='/checkout/shared'"
                 " AND is_current=1 AND project_id=(SELECT id FROM projects"
                 " WHERE name='first-owner')") == 1);

   /* Same checkout, different project name: accepted, not refused. */
   int64_t taker = db2_code_index_project_upsert("second-owner", root);
   assert(taker > 0);
   assert(taker != owner);

   /* The alias now points at the new project, and only at it. */
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE alias='/checkout/shared'") == 1);
   assert(scalar("SELECT COUNT(*) FROM code_project_aliases WHERE alias='/checkout/shared'"
                 " AND project_id=(SELECT id FROM projects WHERE name='second-owner')") == 1);

   /* The previous project still exists; only the checkout moved on. */
   assert(scalar("SELECT COUNT(*) FROM projects WHERE name='first-owner'") == 1);
}

int main(void)
{
   db2_test_shim_open();
   test_move_detach_readd();
   test_manifest_confirmation_and_audit();
   test_gc_audit();
   test_reindex_under_new_name_takes_the_alias();
   db2_test_shim_close();
   puts("code_project_lifecycle: all tests passed");
   return 0;
}
