/* test_cross_repo_route.c: H0d inter-repo route index. Seeds files/file_imports/
 * cross_repo_identity over the sqlite shim and verifies the rebuilt
 * cross_repo_route adjacency (module routes HIGH, header routes MEDIUM, vendored
 * + cross-language + self-edges excluded). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "aimee.h"
#include "db2_test_shim.h"
#include "../db2/cross_repo_route.h"
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

/* count routes matching caller/definer/kind (value-exact). */
static int64_t route_count(const char *caller, const char *definer, const char *kind)
{
   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       db2_conn(),
       "SELECT COUNT(*) FROM cross_repo_route WHERE caller_project = ?1 AND definer_project = ?2 "
       "AND kind = ?3",
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

/* Add a file (language/vendored) to a project; returns nothing (file id resolved
 * by path in later inserts). */
static void mk_file(const char *proj, const char *path, const char *lang, int vendored)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO files (project_id, path, scanned_at, language, vendored) SELECT id, '%s', "
            "'t', '%s', %d FROM projects WHERE name = '%s'",
            path, lang, vendored, proj);
   X(sql);
}

static void mk_import(const char *proj, const char *file, const char *name)
{
   char sql[512];
   snprintf(
       sql, sizeof(sql),
       "INSERT INTO file_imports (file_id, name) SELECT f.id, '%s' FROM files f JOIN projects p "
       "ON p.id = f.project_id WHERE p.name = '%s' AND f.path = '%s'",
       name, proj, file);
   X(sql);
}

static void mk_identity(const char *proj, const char *kind, const char *value)
{
   char sql[512];
   snprintf(sql, sizeof(sql),
            "INSERT INTO cross_repo_identity (project, kind, value) VALUES ('%s', '%s', '%s')",
            proj, kind, value);
   X(sql);
}

static void test_routes(void)
{
   printf("test_routes... ");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('app','/a','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('libhdr','/h','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('gomodlib','/g','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('rustlib','/r','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vend','/v','t','trusted')");

   /* libhdr exports a header that app includes (header route, MEDIUM). */
   mk_file("libhdr", "include/widget.h", "c", 0);
   /* gomodlib + rustlib publish module identities. */
   mk_identity("gomodlib", "gomod", "example.com/gomodlib");
   mk_identity("rustlib", "crate", "rustlib");
   /* vend is a non-vendored repo BUT its header file is in a vendored subtree. */
   mk_file("vend", "third_party/widget.h", "c", 1);

   /* app: a C file that #includes widget.h + go/rust source with module imports. */
   mk_file("app", "src/main.c", "c", 0);
   mk_import("app", "src/main.c", "widget.h"); /* -> libhdr (header) + vend (vendored, excl) */
   mk_file("app", "svc/main.go", "go", 0);
   mk_import("app", "svc/main.go", "example.com/gomodlib/sub"); /* prefix -> gomodlib (module) */
   mk_file("app", "ui/lib.rs", "rust", 0);
   mk_import("app", "ui/lib.rs", "rustlib"); /* exact -> rustlib (module) */
   /* a self-import (app includes its own header) must NOT create a self-route. */
   mk_file("app", "include/self.h", "c", 0);
   mk_import("app", "src/main.c", "self.h");

   /* '_' must be a literal, not a LIKE wildcard: app includes "foo_bar.h"; a
    * definer file "fooXbar.h" must NOT match (it would, unescaped). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('ulib','/u','t','trusted')");
   mk_file("ulib", "inc/fooXbar.h", "c", 0);
   mk_import("app", "src/main.c", "foo_bar.h");

   /* kind/language restriction: a Go file importing a bare CRATE name must NOT
    * match the rust crate identity (go matches only gomod identities). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES "
     "('cratelib','/cr','t','trusted')");
   mk_identity("cratelib", "crate", "cratelib");
   mk_file("app", "svc/y.go", "go", 0);
   mk_import("app", "svc/y.go", "cratelib");

   int rc = db2_cross_repo_rebuild_routes();
   assert(rc >= 3);

   assert(route_count("app", "libhdr", "import_header") == 1);   /* header route */
   assert(route_count("app", "gomodlib", "import_module") == 1); /* gomod prefix match */
   assert(route_count("app", "rustlib", "import_module") == 1);  /* crate exact match */
   assert(route_count("app", "vend", "import_header") == 0);     /* vendored definer excluded */
   assert(route_count("app", "app", "import_header") == 0);      /* no self-route */
   assert(route_count("app", "ulib", "import_header") == 0); /* '_' escaped, no wildcard match */
   assert(route_count("app", "cratelib", "import_module") == 0); /* go!=crate kind/lang mismatch */

   /* idempotent rebuild. */
   int rc2 = db2_cross_repo_rebuild_routes();
   assert(rc2 == rc);
   assert(route_count("app", "libhdr", "import_header") == 1);
   printf("ok\n");
}

int main(void)
{
   db2_test_shim_open();
   test_routes();
   printf("cross_repo_route: all tests passed\n");
   return 0;
}
