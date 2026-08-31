/* db2_test_shim.c: helpers that own the DB2 shim lifecycle so tests
 * do not need to touch sqlite3 directly. See db2_test_shim.h.
 *
 * Compiled only when the shim is enabled (kb builds set
 * AIMEE_DISABLE_DB2_SQLITE_SHIM and skip this translation unit's
 * body). Linked into test binaries via tests/Rules.mk; not included
 * in DB2_SRCS, so production server/CLI builds carry the symbols
 * but never call them. */

#ifndef AIMEE_DISABLE_DB2_SQLITE_SHIM

#include "config_embedder_dims.h" /* the one width declaration (no config link) */
#include "db2_test_shim.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h" /* aimee_pg_* — the postgres-backed mode */
#include "db_schema.h"
#include "lifecycle.h" /* db2_set_embedding_dim */

#include <assert.h>
#include <sqlite3.h>
#include <stddef.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Weakly link to DB1's per-handle statement-cache flush. The shim
 * uses one sqlite handle to back the DB2 surface, and any DB1 helper
 * that prepared a statement against it (test seed paths, stray
 * db1_prepare callers in tests still being migrated) must drop its
 * cached entry before the handle closes. Production binaries that
 * link db1 expose the symbol; standalone test variants without db1
 * resolve it to NULL and skip the call. */
extern void db1_stmt_cache_clear_for_sqlite(struct sqlite3 *db) __attribute__((weak));

static sqlite3 *g_shim_handle;

/* --- Postgres-backed mode -------------------------------------------------
 *
 * The sqlite shim TRANSLATES DB2's SQL rather than executing it, so the places
 * where the two engines genuinely differ — affected-row counts, how the
 * scope-filter macros expand, anything postgres-specific in a query — are
 * exactly the places it cannot vouch for. Setting AIMEE_TEST_DB2_TEMPLATE_URL to
 * a database carrying an applied DB2 schema (see `make db2-test-template`) runs
 * the same tests against the real engine.
 *
 * Isolation is per PROCESS, not per test. Each test binary clones the template
 * into its own `<template>_p<pid>` database, so a parallel `make -j` run cannot
 * have two binaries treading on each other. A clone costs ~200 ms and a drop can
 * stall for seconds behind a checkpoint — affordable once per binary, not 98
 * times. Between tests the far cheaper aimee_test_reset() (installed in the
 * template) returns the database to its freshly-seeded state in ~60 ms. */
static char g_pg_base_url[1024]; /* the template URL, as given */
static char g_pg_test_url[1400]; /* the per-process clone URL */
static char g_pg_test_db[256];   /* the per-process clone database name */
static int g_pg_mode;            /* 1 once the clone is live */
static int g_pg_atexit_registered;

/* The empty database handed to the eval scratch store (db2_test_shim_prepare_eval_store),
 * kept separate from the schema-bearing clone above. */
static char g_pg_eval_db[256];
static char g_pg_eval_url[1400];

/* Split a libpq URL into everything-up-to-and-including the last '/', the
 * database name, and any query suffix. Only the URL form the harness passes is
 * handled: scheme://[user@]host[:port]/dbname[?params]. */
static int pg_split_url(const char *url, char *prefix, size_t prefix_len, char *dbname,
                        size_t dbname_len, char *suffix, size_t suffix_len)
{
   const char *scheme = strstr(url, "://");
   if (!scheme)
      return -1;
   const char *slash = strchr(scheme + 3, '/');
   if (!slash || !slash[1])
      return -1;
   const char *q = strchr(slash + 1, '?');
   size_t plen = (size_t)(slash - url) + 1; /* keep the '/' */
   size_t dlen = q ? (size_t)(q - slash - 1) : strlen(slash + 1);
   if (plen >= prefix_len || dlen >= dbname_len)
      return -1;
   memcpy(prefix, url, plen);
   prefix[plen] = '\0';
   memcpy(dbname, slash + 1, dlen);
   dbname[dlen] = '\0';
   snprintf(suffix, suffix_len, "%s", q ? q : "");
   return 0;
}

/* Run one statement through a short-lived connection. CREATE/DROP DATABASE
 * cannot run on a connection to the database being created or dropped. */
static int pg_admin_exec(const char *url, const char *sql, char *err, size_t err_len)
{
   void *c = aimee_pg_open(url, err, err_len);
   if (!c)
      return -1;
   int rc = aimee_pg_exec(c, sql, err, err_len);
   aimee_pg_close(c);
   return rc;
}

static void pg_drop_clone(void)
{
   if (!g_pg_test_db[0])
      return;
   char prefix[1024], dbname[256], suffix[256];
   if (pg_split_url(g_pg_base_url, prefix, sizeof(prefix), dbname, sizeof(dbname), suffix,
                    sizeof(suffix)) == 0)
   {
      char admin_url[1400];
      snprintf(admin_url, sizeof(admin_url), "%spostgres%s", prefix, suffix);
      char sql[512];
      snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", g_pg_test_db);
      char err[512] = "";
      /* Best-effort: a leaked scratch database is operator noise, not a test
       * result, and failing the run for it would report a defect the code under
       * test did not have. */
      (void)pg_admin_exec(admin_url, sql, err, sizeof(err));
   }
   g_pg_test_db[0] = '\0';
}

static void pg_drop_eval_db(void)
{
   if (!g_pg_eval_db[0])
      return;
   char prefix[1024], dbname[256], suffix[256];
   if (pg_split_url(g_pg_eval_url, prefix, sizeof(prefix), dbname, sizeof(dbname), suffix,
                    sizeof(suffix)) == 0)
   {
      char admin_url[1400];
      snprintf(admin_url, sizeof(admin_url), "%spostgres%s", prefix, suffix);
      char sql[512], err[512] = "";
      snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", g_pg_eval_db);
      (void)pg_admin_exec(admin_url, sql, err, sizeof(err));
   }
   g_pg_eval_db[0] = '\0';
   g_pg_eval_url[0] = '\0';
}

static void pg_atexit(void)
{
   if (g_pg_mode)
   {
      db2_shutdown();
      g_pg_mode = 0;
   }
   pg_drop_clone();
   pg_drop_eval_db();
}

/* A failing test aborts, and abort() does not run atexit handlers — so without
 * this every assertion failure would strand a clone database on the server. That
 * is the common case during a migration, not a rare one: a suite-wide run of
 * failing tests would otherwise leave dozens behind and fill the volume. Drop the
 * clone, then restore the default disposition and re-raise so the signal still
 * reports normally (core dump, shell status). */
static void pg_fatal_signal(int sig)
{
   pg_drop_clone();
   pg_drop_eval_db();
   signal(sig, SIG_DFL);
   raise(sig);
}

static void pg_install_cleanup(void)
{
   if (g_pg_atexit_registered)
      return;
   atexit(pg_atexit);
   static const int fatal[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGINT, SIGTERM};
   for (size_t i = 0; i < sizeof(fatal) / sizeof(fatal[0]); i++)
      signal(fatal[i], pg_fatal_signal);
   g_pg_atexit_registered = 1;
}

/* Clone the template and point DB2 at the copy. Aborts on failure: a run that
 * quietly fell back to sqlite after being asked for Postgres would report a pass
 * that means nothing. */
static void pg_open_clone(const char *template_url)
{
   char prefix[1024], dbname[256], suffix[256];
   if (pg_split_url(template_url, prefix, sizeof(prefix), dbname, sizeof(dbname), suffix,
                    sizeof(suffix)) != 0)
   {
      fprintf(stderr, "db2 test shim: cannot parse AIMEE_TEST_DB2_TEMPLATE_URL (%s)\n",
              template_url);
      abort();
   }

   snprintf(g_pg_base_url, sizeof(g_pg_base_url), "%s", template_url);
   snprintf(g_pg_test_db, sizeof(g_pg_test_db), "%s_p%d", dbname, (int)getpid());

   char admin_url[1400];
   snprintf(admin_url, sizeof(admin_url), "%spostgres%s", prefix, suffix);

   char sql[768];
   char err[512] = "";
   /* A previous run killed mid-test leaves its clone behind; reuse of the pid
    * would then collide with a database this process does not own. */
   snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", g_pg_test_db);
   (void)pg_admin_exec(admin_url, sql, err, sizeof(err));

   snprintf(sql, sizeof(sql), "CREATE DATABASE \"%s\" TEMPLATE \"%s\"", g_pg_test_db, dbname);
   if (pg_admin_exec(admin_url, sql, err, sizeof(err)) != 0)
   {
      fprintf(stderr,
              "db2 test shim: CREATE DATABASE \"%s\" TEMPLATE \"%s\" failed: %s\n"
              "  (build the template first: make db2-test-template)\n",
              g_pg_test_db, dbname, err);
      abort();
   }

   snprintf(g_pg_test_url, sizeof(g_pg_test_url), "%s%s%s", prefix, g_pg_test_db, suffix);
   /* Installed before db2_init, not after: a failure in there aborts, and the
    * handlers are what drop the clone that has already been created. */
   pg_install_cleanup();

   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(CONFIG_EMBEDDER_DIMS_DEFAULT);
   /* The clone already carries the schema. Verify rather than re-apply, so two
    * binaries starting at once cannot collide on "tuple concurrently updated". */
   db2_set_schema_readonly(1);
   if (db2_init(g_pg_test_url) != 0)
   {
      fprintf(stderr, "db2 test shim: db2_init failed against %s\n", g_pg_test_url);
      pg_drop_clone();
      abort();
   }


   g_pg_mode = 1;
}

int db2_test_shim_prepare_eval_store(void)
{
   const char *template_url = getenv("AIMEE_TEST_DB2_TEMPLATE_URL");
   if (!template_url || !template_url[0])
      return 0; /* sqlite shim: the eval store opens an in-memory handle itself */
   if (g_pg_eval_url[0])
      return 0; /* already prepared */

   char prefix[1024], dbname[256], suffix[256];
   if (pg_split_url(template_url, prefix, sizeof(prefix), dbname, sizeof(dbname), suffix,
                    sizeof(suffix)) != 0)
   {
      fprintf(stderr, "db2 test shim: cannot parse AIMEE_TEST_DB2_TEMPLATE_URL (%s)\n",
              template_url);
      return -1;
   }

   /* An EMPTY database, not a clone of the template. db2_eval_open_temp_store_pg
    * carves a throwaway SCHEMA and puts it first on search_path -- but CREATE TABLE
    * IF NOT EXISTS resolves through the whole path, so against a database that
    * already carries the schema in `public` every table is found, skipped, and the
    * following ALTER ... ADD CONSTRAINT then fails on the public copy. The eval
    * store needs somewhere with nothing in it. */
   snprintf(g_pg_eval_db, sizeof(g_pg_eval_db), "%s_eval_p%d", dbname, (int)getpid());

   char admin_url[1400];
   snprintf(admin_url, sizeof(admin_url), "%spostgres%s", prefix, suffix);

   char sql[768];
   char err[512] = "";
   snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", g_pg_eval_db);
   (void)pg_admin_exec(admin_url, sql, err, sizeof(err));
   snprintf(sql, sizeof(sql), "CREATE DATABASE \"%s\"", g_pg_eval_db);
   if (pg_admin_exec(admin_url, sql, err, sizeof(err)) != 0)
   {
      fprintf(stderr, "db2 test shim: CREATE DATABASE \"%s\" failed: %s\n", g_pg_eval_db, err);
      g_pg_eval_db[0] = '\0';
      return -1;
   }

   snprintf(g_pg_eval_url, sizeof(g_pg_eval_url), "%s%s%s", prefix, g_pg_eval_db, suffix);
   setenv("AIMEE_DB2_EVAL_URL", g_pg_eval_url, 1);
   pg_install_cleanup();
   return 0;
}

/* Between tests: restore the freshly-seeded state without paying for a reconnect
 * or another schema apply. */
static void pg_reset(void)
{
   char err[512] = "";
   if (aimee_pg_exec(db2_conn(), "SELECT aimee_test_reset()", err, sizeof(err)) != 0)
   {
      fprintf(stderr, "db2 test shim: aimee_test_reset() failed: %s\n", err);
      abort();
   }
}

int db2_test_shim_is_postgres(void)
{
   return g_pg_mode;
}

int db2_test_shim_skip_on_postgres(const char *test_name)
{
   /* Keyed off the environment, not g_pg_mode: callers check this at the top of
    * main(), before any shim open has set g_pg_mode, precisely so they can bail
    * out before touching a handle that will not exist. */
   const char *url = getenv("AIMEE_TEST_DB2_TEMPLATE_URL");
   if (!g_pg_mode && !(url && url[0]))
      return 0;
   printf("  SKIP %s: seeds through the raw sqlite handle (postgres mode)\n",
          test_name ? test_name : "test");
   return 1;
}

void db2_test_shim_open(void)
{
   db2_test_shim_open_path(":memory:");
}

void db2_test_shim_open_path(const char *path)
{
   const char *template_url = getenv("AIMEE_TEST_DB2_TEMPLATE_URL");
   if (template_url && template_url[0])
   {
      if (g_pg_mode)
         pg_reset(); /* already connected: the reset IS the reopen */
      else
         pg_open_clone(template_url);
      return;
   }

#ifdef AIMEE_TEST_PG_BACKEND
   /* Built with AIMEE_TEST_PG=1, so aimee_pg_* IS libpq and the sqlite path below
    * cannot work: db2_init("shim") would try to reach a database literally named
    * "shim" and the assert would fire with nothing explaining why. Say what is
    * actually wrong instead. */
   fprintf(stderr, "db2 test shim: this binary was built with AIMEE_TEST_PG=1 (real libpq),\n"
                   "  but AIMEE_TEST_DB2_TEMPLATE_URL is unset, so there is no database to\n"
                   "  clone. Set it, or rebuild without AIMEE_TEST_PG for the sqlite shim.\n");
   abort();
#endif

   db2_test_shim_close();

   sqlite3 *raw = NULL;
   int rc = sqlite3_open(path && *path ? path : ":memory:", &raw);
   assert(rc == SQLITE_OK && raw != NULL);

   /* Foreign keys must be on for cascade-delete schema to behave like
    * production; the shim schema relies on it (memory_workspaces FK to
    * memories, etc.). */
   sqlite3_exec(raw, "PRAGMA foreign_keys=ON", NULL, NULL, NULL);

   /* Unit tests embed with the builtin embedder, which fills the DEPLOYMENT's width.
    * Take that width from config — the one place it is declared — so the shim and the
    * builtin cannot disagree; a literal here was a second declaration. Tests that
    * exercise a specific width call db2_set_embedding_dim themselves after open. */
   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(CONFIG_EMBEDDER_DIMS_DEFAULT);

   char err[512] = {0};
   rc = db2_apply_schema_sqlite_shim(raw, err, sizeof(err));
   assert(rc == 0);
   /* Production registers this host contract during KB module startup. The
    * shim owns the equivalent test startup boundary, so mutation paths retain
    * their mandatory WORM audit without every fixture reimplementing boot. */

   db2_register_shared_sqlite(raw);
   rc = db2_init("shim");
   assert(rc == 0);

   g_shim_handle = raw;
}

void db2_test_shim_close(void)
{
   /* In postgres mode the connection outlives the individual test — reconnecting
    * per test would cost another clone. pg_atexit does the real teardown. */
   if (g_pg_mode)
      return;

   if (!g_shim_handle)
   {
      db2_register_shared_sqlite(NULL);
      return;
   }

   db2_shutdown();
   if (db1_stmt_cache_clear_for_sqlite)
      db1_stmt_cache_clear_for_sqlite(g_shim_handle);
   db2_register_shared_sqlite(NULL);
   sqlite3_close(g_shim_handle);
   g_shim_handle = NULL;
}

void *db2_test_shim_handle(void)
{
   return g_shim_handle;
}

#endif /* !AIMEE_DISABLE_DB2_SQLITE_SHIM */
