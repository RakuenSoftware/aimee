/* test_cross_repo_stats.c: S3 DB stats layer over the sqlite shim. Seeds a tiny
 * multi-repo corpus and exercises distinctiveness stats, blocked_symbols
 * recompute/membership, per-repo + repo-set hashes, and meta read. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_stats.h"
#include "../db2/db2.h"
#include "../db2/db_postgres.h"

static void X(const char *sql)
{
   char err[256] = "";
   int rc = aimee_pg_exec(db2_conn(), sql, err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "seed failed: %s\n  sql: %s\n", err, sql);
   assert(rc == 0);
}

/* Seed: trusted repos A (caller), B + C (both define LiStartConnection -> 2
 * definers), plus a "render" callee/def spread across A,B,C,U and an untrusted
 * repo U whose defs must NOT count toward trusted frequency. */
static void seed(void)
{
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('A','/a','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('B','/b','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('C','/c','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('U','/u','t','untrusted')");
   /* files: A has 4 files (f1..f4); B,C,U one each. */
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'a1.c','t' FROM projects WHERE "
     "name='A'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'a2.c','t' FROM projects WHERE "
     "name='A'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'a3.c','t' FROM projects WHERE "
     "name='A'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'a4.c','t' FROM projects WHERE "
     "name='A'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'b.c','t' FROM projects WHERE "
     "name='B'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'c.c','t' FROM projects WHERE "
     "name='C'");
   X("INSERT INTO files (project_id,path,scanned_at) SELECT id,'u.c','t' FROM projects WHERE "
     "name='U'");
   /* definitions: LiStartConnection defined in B and C (2 trusted definers). */
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'LiStartConnection','definition' FROM files "
     "WHERE path='b.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'LiStartConnection','definition' FROM files "
     "WHERE path='c.c'");
   /* render defined in A,B,C (3 trusted) + U (untrusted, must not count). */
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'render','definition' FROM files WHERE "
     "path='a1.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'render','definition' FROM files WHERE "
     "path='b.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'render','definition' FROM files WHERE "
     "path='c.c'");
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'render','definition' FROM files WHERE "
     "path='u.c'");
   /* a route row must be ignored as a definition. */
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'GET /x','route' FROM files WHERE "
     "path='b.c'");
   /* calls: A calls LiStartConnection in 1 of its 4 files; render called in A's a1,a2 + B,C. */
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'LiStartConnection' FROM files WHERE "
     "path='a1.c'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'render' FROM files WHERE path='a1.c'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'render' FROM files WHERE path='a2.c'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'render' FROM files WHERE path='b.c'");
   X("INSERT INTO code_calls (file_id,callee) SELECT id,'render' FROM files WHERE path='c.c'");
   X("INSERT INTO file_exports (file_id,name) SELECT id,'LiStartConnection' FROM files WHERE "
     "path='b.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT id,'Limelight.h' FROM files WHERE "
     "path='a1.c'");
}

static void test_distinct_stats(void)
{
   printf("test_distinct_stats... ");
   xrepo_distinct_stats_t s;
   /* LiStartConnection: defined in B,C (2 trusted); called in A only (1 repo);
    * A uses it in 1 of 4 files -> 25%. */
   assert(db2_cross_repo_distinct_stats("LiStartConnection", "A", &s) == 0);
   assert(s.definer_repo_count == 2);
   assert(s.callee_repo_count == 1);
   assert(s.caller_file_pct == 25);

   /* render: defined in A,B,C trusted (3, NOT U); called in A,B,C (3 repos);
    * A uses it in a1,a2 -> 2 of 4 files = 50%. */
   assert(db2_cross_repo_distinct_stats("render", "A", &s) == 0);
   assert(s.definer_repo_count == 3); /* untrusted U excluded */
   assert(s.callee_repo_count == 3);
   assert(s.caller_file_pct == 50);
   printf("ok\n");
}

static void test_blocked_symbols(void)
{
   printf("test_blocked_symbols... ");
   /* k=3 (callee in >=3 repos), m=3 (defined in >=3 repos), len_min=4.
    * render: callee in 3 + defined in 3 -> blocked. LiStartConnection: callee 1,
    * defined 2 -> not blocked. */
   int n = db2_cross_repo_recompute_blocked_symbols(3, 3, 4);
   assert(n >= 1);
   assert(db2_cross_repo_symbol_blocked("render", "") == 1);
   assert(db2_cross_repo_symbol_blocked("LiStartConnection", "") == 0);
   /* version bumped in meta. */
   int64_t ver = 0;
   assert(db2_cross_repo_meta_read(NULL, &ver, NULL, 0) == 0);
   assert(ver >= 1);
   /* recompute again -> version strictly increases (deterministic bump). */
   int64_t ver2 = 0;
   db2_cross_repo_recompute_blocked_symbols(3, 3, 4);
   db2_cross_repo_meta_read(NULL, &ver2, NULL, 0);
   assert(ver2 == ver + 1);
   printf("ok\n");
}

static void test_hashes(void)
{
   printf("test_hashes... ");
   char ha[17] = "", hb[17] = "", ha2[17] = "";
   assert(db2_cross_repo_repo_symbol_hash("A", ha, sizeof(ha)) == 0);
   assert(db2_cross_repo_repo_symbol_hash("B", hb, sizeof(hb)) == 0);
   assert(strlen(ha) == 16 && strlen(hb) == 16);
   assert(strcmp(ha, hb) != 0); /* different repos -> different hashes */
   /* deterministic: same inputs -> same hash. */
   assert(db2_cross_repo_repo_symbol_hash("A", ha2, sizeof(ha2)) == 0);
   assert(strcmp(ha, ha2) == 0);

   char rs[17] = "", rs2[17] = "";
   assert(db2_cross_repo_repo_set_hash(rs, sizeof(rs)) == 0);
   assert(strlen(rs) == 16);
   /* persisted into meta. */
   char stored[64] = "";
   assert(db2_cross_repo_meta_read(NULL, NULL, stored, sizeof(stored)) == 0);
   assert(strcmp(stored, rs) == 0);
   /* a symbol change in A bumps A's hash and the repo-set hash. */
   X("INSERT INTO terms (file_id,name,kind) SELECT id,'NewlyAddedSym','definition' FROM files "
     "WHERE path='a3.c'");
   char ha3[17] = "";
   assert(db2_cross_repo_repo_symbol_hash("A", ha3, sizeof(ha3)) == 0);
   assert(strcmp(ha, ha3) != 0);
   assert(db2_cross_repo_repo_set_hash(rs2, sizeof(rs2)) == 0);
   assert(strcmp(rs, rs2) != 0);
   printf("ok\n");
}

static void test_no_conn_graceful(void)
{
   printf("test_no_conn_graceful... ");
   /* NULL inputs degrade, never crash. */
   xrepo_distinct_stats_t s;
   assert(db2_cross_repo_distinct_stats(NULL, "A", &s) == -1);
   assert(db2_cross_repo_recompute_blocked_symbols(0, 3, 4) == -1); /* bad threshold */
   char buf[17];
   assert(db2_cross_repo_repo_symbol_hash("A", buf, 4) == -1); /* cap too small */
   printf("ok\n");
}

/* count cross_repo_trust_audit rows for a project (direct shim query). */
static int64_t audit_count(const char *project)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM cross_repo_trust_audit WHERE project = ?1", err,
       sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", project);
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* S7: per-repo trust write + audit. Runs last — it flips A's trust permanently. */
static void test_set_trust(void)
{
   int64_t epoch0 = 0;
   db2_cross_repo_meta_read(&epoch0, NULL, NULL, 0);
   assert(audit_count("A") == 0);

   /* trusted -> untrusted: applied, changed, epoch bumps, audited */
   char prior[16] = "";
   int changed = -1;
   assert(db2_cross_repo_set_trust("A", "untrusted", "tester", "r1", prior, sizeof(prior),
                                   &changed) == 0);
   assert(strcmp(prior, "trusted") == 0 && changed == 1);
   int64_t epoch1 = 0;
   db2_cross_repo_meta_read(&epoch1, NULL, NULL, 0);
   assert(epoch1 == epoch0 + 1);
   assert(audit_count("A") == 1);

   /* no-op re-assert: persisted (prior now untrusted), NOT changed, epoch stable,
    * still audited (every call records the actor's action). */
   changed = -1;
   assert(db2_cross_repo_set_trust("A", "untrusted", "tester", "r2", prior, sizeof(prior),
                                   &changed) == 0);
   assert(strcmp(prior, "untrusted") == 0 && changed == 0);
   int64_t epoch2 = 0;
   db2_cross_repo_meta_read(&epoch2, NULL, NULL, 0);
   assert(epoch2 == epoch1);
   assert(audit_count("A") == 2);

   /* no such project / bad trust value -> no audit, no epoch change */
   assert(db2_cross_repo_set_trust("ZZZ", "trusted", "t", "r3", NULL, 0, NULL) == 1);
   assert(db2_cross_repo_set_trust("A", "bogus", "t", "r4", NULL, 0, NULL) == -1);
   int64_t epoch3 = 0;
   db2_cross_repo_meta_read(&epoch3, NULL, NULL, 0);
   assert(epoch3 == epoch2);
   assert(audit_count("A") == 2);
   assert(audit_count("ZZZ") == 0);
}

int main(void)
{
   db2_test_shim_open();
   seed();
   test_distinct_stats();
   test_blocked_symbols();
   test_hashes();
   test_no_conn_graceful();
   test_set_trust();
   printf("cross_repo_stats: all tests passed\n");
   return 0;
}
