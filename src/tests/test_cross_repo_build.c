/* test_cross_repo_build.c: recall-recovery R2b. Pure build-dep extraction +
 * URL->repo mapping, and the cross_repo_build_dep rebuild over the sqlite shim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_build.h"
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

static int64_t bd_count(const char *caller, const char *definer, const char *kind)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT COUNT(*) FROM cross_repo_build_dep WHERE caller_project=?1 AND definer_project=?2 "
       "AND build_kind=?3",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", caller);
   aimee_pg_bind_text(st, "?2", definer);
   aimee_pg_bind_text(st, "?3", kind);
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

static void test_ref_repo(void)
{
   printf("test_ref_repo... ");
   char r[128];
   assert(xrepo_build_ref_repo("https://github.com/games-on-whales/inputtino.git", r, sizeof(r)));
   assert(strcmp(r, "inputtino") == 0);
   assert(
       xrepo_build_ref_repo("https://u:tok@github.com/owner/Moonlight-Common-C.git", r, sizeof(r)));
   assert(strcmp(r, "moonlight-common-c") == 0); /* userinfo stripped, lowercased */
   assert(xrepo_build_ref_repo("git@github.com:owner/mdns_cpp.git", r, sizeof(r)));
   assert(strcmp(r, "mdns_cpp") == 0); /* scp form */
   assert(xrepo_build_ref_repo("../inputtino", r, sizeof(r)));
   assert(strcmp(r, "inputtino") == 0); /* path dep */
   assert(xrepo_build_ref_repo("https://github.com/o/r/", r, sizeof(r)));
   assert(strcmp(r, "r") == 0); /* trailing slash */
   printf("ok\n");
}

static void test_extract(void)
{
   printf("test_extract... ");
   xrepo_build_dep_t d[16];
   int ovf = 0;
   /* CMake FetchContent GIT_REPOSITORY (multi-line) */
   const char *cmake = "FetchContent_Declare(\n  inputtino\n  GIT_REPOSITORY "
                       "https://github.com/games-on-whales/inputtino.git\n  GIT_TAG main)\n"
                       "FetchContent_Declare(varied GIT_REPOSITORY ${SOME_VAR}/x.git)\n";
   int n = xrepo_extract_build_deps("repo/CMakeLists.txt", cmake, d, 16, &ovf);
   assert(n == 2);
   int seen_inp = 0, seen_var = 0;
   for (int i = 0; i < n; i++)
   {
      assert(strcmp(d[i].kind, "fetchcontent") == 0);
      if (strstr(d[i].ref, "inputtino"))
      {
         seen_inp = 1;
         assert(d[i].low_conf == 0);
      }
      if (strstr(d[i].ref, "${SOME_VAR}"))
      {
         seen_var = 1;
         assert(d[i].low_conf == 1); /* ${VAR} -> low parse confidence */
      }
   }
   assert(seen_inp && seen_var);
   /* .gitmodules */
   const char *gm = "[submodule \"x\"]\n\tpath = a/b\n\turl = "
                    "https://github.com/moonlight-stream/moonlight-common-c.git\n";
   n = xrepo_extract_build_deps(".gitmodules", gm, d, 16, &ovf);
   assert(n == 1 && strcmp(d[0].kind, "submodule") == 0 && strstr(d[0].ref, "moonlight-common-c"));
   /* Cargo git + path */
   const char *cargo = "[dependencies]\nfoo = { git = \"https://x/owner/smithay.git\" }\n"
                       "bar = { path = \"../wayland-display-core\" }\n";
   n = xrepo_extract_build_deps("Cargo.toml", cargo, d, 16, &ovf);
   assert(n == 2);
   for (int i = 0; i < n; i++)
      assert(strcmp(d[i].kind, "manifest") == 0);

   /* comment/string awareness: a commented-out or string-embedded GIT_REPOSITORY is
    * NOT a dep (only the live one is). */
   const char *cmt = "# FetchContent_Declare(x GIT_REPOSITORY https://h/o/commented.git)\n"
                     "#[[ GIT_REPOSITORY https://h/o/blockcmt.git ]]\n"
                     "set(DOC \"GIT_REPOSITORY https://h/o/instring.git\")\n"
                     "FetchContent_Declare(real GIT_REPOSITORY https://h/o/realdep.git)\n";
   n = xrepo_extract_build_deps("CMakeLists.txt", cmt, d, 16, &ovf);
   assert(n == 1 && strstr(d[0].ref, "realdep"));
   /* TOML line comment: a commented git= is ignored. */
   const char *ccmt = "# foo = { git = \"https://h/o/commented.git\" }\n"
                      "bar = { git = \"https://h/o/realcrate.git\" }\n";
   n = xrepo_extract_build_deps("Cargo.toml", ccmt, d, 16, &ovf);
   assert(n == 1 && strstr(d[0].ref, "realcrate"));
   printf("ok\n");
}

static void test_rebuild(void)
{
   printf("test_rebuild... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('bapp','/ba','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('inputtino','/in','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('moonlight-common-c','/mc','t','trusted')");
   /* bapp's CMakeLists FetchContents inputtino; .gitmodules submodules moonlight-common-c;
    * also references an EXTERNAL repo (fmt) that is not in the corpus -> dropped. */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT id,'CMakeLists.txt',"
     "'t','',0 FROM projects WHERE name='bapp'");
   X("INSERT INTO file_contents (file_id,content) SELECT id,"
     "'FetchContent_Declare(inputtino GIT_REPOSITORY "
     "https://github.com/games-on-whales/inputtino.git)"
     " FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git)' "
     "FROM files WHERE path='CMakeLists.txt' AND project_id=(SELECT id FROM projects WHERE "
     "name='bapp')");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT id,'.gitmodules','t',"
     "'',0 FROM projects WHERE name='bapp'");
   X("INSERT INTO file_contents (file_id,content) SELECT id,"
     "'[submodule \"m\"]\n"
     "  url = https://github.com/moonlight-stream/moonlight-common-c.git\n' "
     "FROM files WHERE path='.gitmodules' AND project_id=(SELECT id FROM projects WHERE "
     "name='bapp')");
   /* a VENDORED manifest must be ignored (declares the vendored lib's deps, not bapp's). */
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'third_party/x/CMakeLists.txt','t','',1 FROM projects WHERE name='bapp'");
   X("INSERT INTO file_contents (file_id,content) SELECT id,"
     "'FetchContent_Declare(inputtino GIT_REPOSITORY "
     "https://github.com/games-on-whales/inputtino.git)' "
     "FROM files WHERE path='third_party/x/CMakeLists.txt' AND project_id=(SELECT id FROM projects "
     "WHERE name='bapp')");

   int rc = db2_cross_repo_rebuild_build_deps();
   assert(rc >= 2);
   assert(bd_count("bapp", "inputtino", "fetchcontent") == 1); /* FetchContent -> corpus repo */
   assert(bd_count("bapp", "moonlight-common-c", "submodule") == 1); /* submodule -> corpus repo */
   /* fmt is external (no corpus project) -> no row; vendored manifest ignored (only 1
    * fetchcontent). */
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(), "SELECT COUNT(*) FROM cross_repo_build_dep WHERE caller_project='bapp'", err,
       sizeof(err));
   assert(st);
   int64_t total = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      total = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   assert(total == 2); /* exactly inputtino + moonlight-common-c; fmt + vendored excluded */
   /* idempotent */
   int rc2 = db2_cross_repo_rebuild_build_deps();
   assert(rc2 == rc);
   printf("ok\n");
}

int main(void)
{
   test_ref_repo();
   test_extract();
   db2_test_shim_open();
   test_rebuild();
   printf("cross_repo_build: all tests passed\n");
   return 0;
}
