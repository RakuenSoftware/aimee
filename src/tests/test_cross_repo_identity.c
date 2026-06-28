/* test_cross_repo_identity.c: H0c repo-identity layer. Unit-tests the pure
 * manifest -> identity extraction and the end-to-end rebuild over the sqlite shim. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_identity.h"
#include "../db2/db2.h"
#include "../db2/db_postgres.h"

static const xrepo_identity_t *find_id(const xrepo_identity_t *ids, int n, const char *kind,
                                       const char *value)
{
   for (int i = 0; i < n; i++)
      if (strcmp(ids[i].kind, kind) == 0 && strcmp(ids[i].value, value) == 0)
         return &ids[i];
   return NULL;
}

static void test_extract_pure(void)
{
   printf("test_extract_pure... ");
   xrepo_identity_t ids[16];

   int n = xrepo_extract_identities("Cargo.toml", "[package]\nname = \"my_crate\"\nversion=\"1\"\n",
                                    ids, 16, NULL);
   assert(n == 1 && find_id(ids, n, "crate", "my_crate"));

   n = xrepo_extract_identities("go.mod", "module example.com/foo\n\ngo 1.21\n", ids, 16, NULL);
   assert(n == 1 && find_id(ids, n, "gomod", "example.com/foo"));

   n = xrepo_extract_identities("package.json", "{\n  \"name\": \"@scope/widgets\"\n}\n", ids, 16,
                                NULL);
   assert(n == 1 && find_id(ids, n, "npm", "@scope/widgets"));

   n = xrepo_extract_identities("pyproject.toml", "[project]\nname = \"requests_ext\"\n", ids, 16,
                                NULL);
   assert(n == 1 && find_id(ids, n, "pypi", "requests_ext"));

   /* CMake: project() + each add_library/add_executable target; ${VAR} skipped;
    * whitespace before '(' (project (X)), a namespaced ALIAS (Foo::Bar), and a
    * target whose name spans a newline after '(' are all handled. */
   n = xrepo_extract_identities(
       "CMakeLists.txt",
       "cmake_minimum_required(VERSION 3.10)\nproject (MoonlightCommonC VERSION 1.2)\n"
       "add_library(moonlight-common-c STATIC src/a.c)\nadd_executable(demo demo.c)\n"
       "add_library(Foo::Bar ALIAS foo)\nadd_library(\n  newline_target STATIC y.c)\n"
       "add_library(${DYN_NAME} SHARED x.c)\n",
       ids, 16, NULL);
   assert(find_id(ids, n, "cmake_project", "MoonlightCommonC")); /* space before ( */
   assert(find_id(ids, n, "cmake_target", "moonlight-common-c"));
   assert(find_id(ids, n, "cmake_target", "demo"));
   assert(find_id(ids, n, "cmake_target", "Foo::Bar"));       /* namespaced alias */
   assert(find_id(ids, n, "cmake_target", "newline_target")); /* arg after newline */
   assert(!find_id(ids, n, "cmake_target", "${DYN_NAME}"));   /* variable skipped */

   /* pkg-config: name = the .pc file basename. */
   n = xrepo_extract_identities("lib/pkgconfig/libfoo.pc", "Name: Foo\nVersion: 1\n", ids, 16,
                                NULL);
   assert(n == 1 && find_id(ids, n, "pkgconfig", "libfoo"));

   /* unknown / empty */
   assert(xrepo_extract_identities("README.md", "hi", ids, 16, NULL) == 0);
   assert(xrepo_extract_identities(NULL, "x", ids, 16, NULL) == 0);
   printf("ok\n");
}

static void X(const char *sql)
{
   char err[256] = "";
   int rc = aimee_pg_exec(db2_conn(), sql, err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "seed failed: %s\n  sql: %s\n", err, sql);
   assert(rc == 0);
}

/* count cross_repo_identity rows matching project/kind/value (any field "" = wildcard). */
static int64_t id_count(const char *project, const char *kind, const char *value)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT COUNT(*) FROM cross_repo_identity WHERE project = ?1 AND kind = ?2 AND value = ?3",
       err, sizeof(err));
   assert(st);
   aimee_pg_bind_text(st, "?1", project);
   aimee_pg_bind_text(st, "?2", kind);
   aimee_pg_bind_text(st, "?3", value);
   int64_t n = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      n = aimee_pg_column_int64(st, 0);
   aimee_pg_finalize(st);
   return n;
}

/* Seed a manifest file with content into project `proj` (vendored 0/1). */
static void seed_manifest_v(const char *proj, const char *path, const char *content, int vendored)
{
   char sql[2048];
   snprintf(sql, sizeof(sql),
            "INSERT INTO files (project_id, path, scanned_at, vendored) SELECT id, '%s', 't', %d "
            "FROM projects WHERE name = '%s'",
            path, vendored, proj);
   X(sql);
   snprintf(sql, sizeof(sql),
            "INSERT INTO file_contents (file_id, content) SELECT f.id, '%s' FROM files f JOIN "
            "projects p ON p.id = f.project_id WHERE p.name = '%s' AND f.path = '%s'",
            content, proj, path);
   X(sql);
}

static void seed_manifest(const char *proj, const char *path, const char *content)
{
   seed_manifest_v(proj, path, content, 0);
}

static void test_rebuild_end_to_end(void)
{
   printf("test_rebuild_end_to_end... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('rustlib','/r','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('clib','/c','t','trusted')");
   seed_manifest("rustlib", "Cargo.toml", "[package]\nname = \"serde_helpers\"\n");
   seed_manifest("clib", "CMakeLists.txt", "project(CLib)\nadd_library(clib_core STATIC a.c)\n");
   seed_manifest("clib", "lib/clib.pc", "Name: clib\n");
   /* a VENDORED manifest must NOT register its crate under the host repo (§4). */
   seed_manifest_v("clib", "third_party/vlib/Cargo.toml", "[package]\nname = \"vendored_crate\"\n",
                   1);

   int rc = db2_cross_repo_rebuild_identities();
   assert(rc >= 4); /* serde_helpers + CLib + clib_core + clib */
   assert(id_count("rustlib", "crate", "serde_helpers") == 1);
   assert(id_count("clib", "crate", "vendored_crate") == 0); /* vendored, excluded */
   assert(id_count("clib", "cmake_project", "CLib") == 1);
   assert(id_count("clib", "cmake_target", "clib_core") == 1);
   assert(id_count("clib", "pkgconfig", "clib") == 1);
   /* cross-project isolation */
   assert(id_count("rustlib", "cmake_project", "CLib") == 0);

   /* idempotent: a second rebuild yields the same set (no dups, DELETE+rebuild). */
   int rc2 = db2_cross_repo_rebuild_identities();
   assert(rc2 == rc);
   assert(id_count("clib", "cmake_target", "clib_core") == 1);
   printf("ok\n");
}

int main(void)
{
   test_extract_pure();
   db2_test_shim_open();
   test_rebuild_end_to_end();
   printf("cross_repo_identity: all tests passed\n");
   return 0;
}
