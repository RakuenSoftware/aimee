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

/* §2 header-basename IDF (H3b): a quoted include whose basename resolves to
 * non-vendored files in >= 4 distinct repos is too ubiquitous to be a confident
 * route (produces none); a basename in < 4 repos still routes. */
static void test_header_idf(void)
{
   printf("test_header_idf... ");
   /* idfapp includes "ubiq.h" and "rare.h". ubiq.h exists in 4 definer repos (>=4
    * distinct => ubiquitous, no route); rare.h in 1 (routes). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('idfapp','/ia','t','trusted')");
   for (int i = 0; i < 4; i++)
   {
      char nm[32], sql[400];
      snprintf(nm, sizeof(nm), "ubiqlib%d", i);
      snprintf(sql, sizeof(sql),
               "INSERT INTO projects (name, root, scanned_at, trust) VALUES ('%s','/u%d','t',"
               "'trusted')",
               nm, i);
      X(sql);
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'inc/ubiq.h','t','c',0 FROM projects WHERE name='%s'",
               nm);
      X(sql);
   }
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('rarelib','/rl','t','trusted')");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT id,'inc/rare.h','t',"
     "'c',0 FROM projects WHERE name='rarelib'");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'src/m.c','t','c',0 "
     "FROM projects WHERE name='idfapp'");
   X("INSERT INTO file_imports (file_id,name) SELECT f.id,'ubiq.h' FROM files f JOIN projects p ON "
     "p.id=f.project_id WHERE p.name='idfapp' AND f.path='src/m.c'");
   X("INSERT INTO file_imports (file_id,name) SELECT f.id,'rare.h' FROM files f JOIN projects p ON "
     "p.id=f.project_id WHERE p.name='idfapp' AND f.path='src/m.c'");

   /* boundary: "bnd.h" in exactly 3 repos (< 4) MUST still route. */
   for (int i = 0; i < 3; i++)
   {
      char nm[32], sql[400];
      snprintf(nm, sizeof(nm), "bndlib%d", i);
      snprintf(sql, sizeof(sql),
               "INSERT INTO projects (name, root, scanned_at, trust) VALUES ('%s','/b%d','t',"
               "'trusted')",
               nm, i);
      X(sql);
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'inc/bnd.h','t','c',0 FROM projects WHERE name='%s'",
               nm);
      X(sql);
   }
   X("INSERT INTO file_imports (file_id,name) SELECT f.id,'bnd.h' FROM files f JOIN projects p ON "
     "p.id=f.project_id WHERE p.name='idfapp' AND f.path='src/m.c'");
   /* vendored copies must NOT count toward ubiquity: "vd.h" in 1 non-vendored repo +
    * 5 vendored repos -> count is 1 (< 4) -> routes to the non-vendored one. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vdreal','/vr','t','trusted')");
   X("INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
     "id,'inc/vd.h','t','c',"
     "0 FROM projects WHERE name='vdreal'");
   for (int i = 0; i < 5; i++)
   {
      char nm[32], sql[400];
      snprintf(nm, sizeof(nm), "vdvend%d", i);
      snprintf(sql, sizeof(sql),
               "INSERT INTO projects (name, root, scanned_at, trust) VALUES ('%s','/vv%d','t',"
               "'trusted')",
               nm, i);
      X(sql);
      snprintf(sql, sizeof(sql),
               "INSERT INTO files (project_id,path,scanned_at,language,vendored) SELECT "
               "id,'third_party/vd.h','t','c',1 FROM projects WHERE name='%s'",
               nm);
      X(sql);
   }
   X("INSERT INTO file_imports (file_id,name) SELECT f.id,'vd.h' FROM files f JOIN projects p ON "
     "p.id=f.project_id WHERE p.name='idfapp' AND f.path='src/m.c'");

   int rc = db2_cross_repo_rebuild_routes();
   assert(rc >= 0);
   /* ubiq.h is in 4 repos => no route to any of them. */
   for (int i = 0; i < 4; i++)
   {
      char nm[32];
      snprintf(nm, sizeof(nm), "ubiqlib%d", i);
      assert(route_count("idfapp", nm, "import_header") == 0);
   }
   /* rare.h is in 1 repo => routes. */
   assert(route_count("idfapp", "rarelib", "import_header") == 1);
   /* bnd.h in exactly 3 repos (< 4) => still routes to all three. */
   for (int i = 0; i < 3; i++)
   {
      char nm[32];
      snprintf(nm, sizeof(nm), "bndlib%d", i);
      assert(route_count("idfapp", nm, "import_header") == 1);
   }
   /* vd.h: 5 vendored copies don't count; the 1 non-vendored repo routes. */
   assert(route_count("idfapp", "vdreal", "import_header") == 1);
   printf("ok\n");
}

/* H5 (H4 live findings): prefer-local header resolution + generated-header reject.
 * (A) when the CALLER repo has its own file matching the include, no cross-repo
 * route is formed (the include resolves locally). (B) generated/build headers
 * (config.h, version.h, ...) never form a cross-repo route even with no local copy. */
static void test_prefer_local_and_generated(void)
{
   printf("test_prefer_local_and_generated... ");
   /* (A) plapp includes "shared.h" AND has its OWN shared.h; pllib also has shared.h.
    * Prefer-local => NO route plapp->pllib. plapp2 has NO local shared.h => routes. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('plapp','/pa','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('plapp2','/pb','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('pllib','/pl','t','trusted')");
   mk_file("pllib", "include/shared.h", "c", 0);
   mk_file("plapp", "src/a.c", "c", 0);
   mk_file("plapp", "include/shared.h", "c", 0); /* caller's OWN copy */
   mk_import("plapp", "src/a.c", "shared.h");
   mk_file("plapp2", "src/b.c", "c", 0); /* no local shared.h */
   mk_import("plapp2", "src/b.c", "shared.h");

   /* (B) genapp includes "config.h"; genlib has config.h; genapp has NO local copy.
    * Generated-header reject => NO route despite the basename being in < 4 repos. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('genapp','/ga','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('genlib','/gl','t','trusted')");
   mk_file("genlib", "include/config.h", "c", 0);
   mk_file("genapp", "src/c.c", "c", 0);
   mk_import("genapp", "src/c.c", "config.h");

   /* (B2) path-qualified "sub/config.h" is NOT a bare generated header => routes. */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('genapp2','/g2','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('genlib2','/l2','t','trusted')");
   mk_file("genlib2", "inc/sub/config.h", "c", 0);
   mk_file("genapp2", "src/d.c", "c", 0);
   mk_import("genapp2", "src/d.c", "sub/config.h");

   /* (A-vendored) vcapp includes "vlib.h" and has its OWN copy but VENDORED; vclib
    * has it non-vendored. fl.vendored=0 => the vendored caller copy is NOT local =>
    * route to vclib still forms (a caller that vendors a lib still routes to it). */
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vcapp','/vca','t','trusted')");
   X("INSERT INTO projects (name, root, scanned_at, trust) VALUES ('vclib','/vcl','t','trusted')");
   mk_file("vclib", "include/vlib.h", "c", 0);
   mk_file("vcapp", "src/e.c", "c", 0);
   mk_file("vcapp", "third_party/vlib.h", "c", 1); /* caller's copy is VENDORED */
   mk_import("vcapp", "src/e.c", "vlib.h");

   int rc = db2_cross_repo_rebuild_routes();
   assert(rc >= 0);
   assert(route_count("plapp", "pllib", "import_header") == 0);   /* prefer-local: caller owns it */
   assert(route_count("plapp2", "pllib", "import_header") == 1);  /* no local copy: routes */
   assert(route_count("genapp", "genlib", "import_header") == 0); /* bare config.h rejected */
   assert(route_count("genapp2", "genlib2", "import_header") ==
          1);                                                   /* path-qualified: NOT rejected */
   assert(route_count("vcapp", "vclib", "import_header") == 1); /* vendored caller copy != local */
   printf("ok\n");
}

int main(void)
{
   db2_test_shim_open();
   test_routes();
   test_header_idf();
   test_prefer_local_and_generated();
   printf("cross_repo_route: all tests passed\n");
   return 0;
}
