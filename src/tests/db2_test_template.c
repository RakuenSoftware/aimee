/* db2_test_template.c: build the Postgres template database the DB2 test shim
 * clones from (see db2_test_shim.c's postgres-backed mode).
 *
 * Applying the schema costs the better part of a second and only has to happen
 * once per run, not once per test binary — so it happens here, and every test
 * process gets a cheap `CREATE DATABASE ... TEMPLATE` copy instead.
 *
 *   usage: db2-test-template <template-url> <reset-sql-path>
 *
 * Drops and recreates <template-url>'s database, applies the DB2 schema to it,
 * installs the reset helpers from <reset-sql-path>, then snapshots whatever rows
 * the schema seeded so aimee_test_reset() can restore them between tests. */
#include "aimee.h"
#include "config_embedder_dims.h"
#include "modules/db2/c/db2.h"
#include "modules/db2/c/db_postgres.h"
#include "modules/db2/c/lifecycle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TPL_ERRBUF 1024

/* Split scheme://[user@]host[:port]/dbname[?params] the same way the shim does. */
static int split_url(const char *url, char *prefix, size_t prefix_len, char *dbname,
                     size_t dbname_len, char *suffix, size_t suffix_len)
{
   const char *scheme = strstr(url, "://");
   if (!scheme)
      return -1;
   const char *slash = strchr(scheme + 3, '/');
   if (!slash || !slash[1])
      return -1;
   const char *q = strchr(slash + 1, '?');
   size_t plen = (size_t)(slash - url) + 1;
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

static int admin_exec(const char *url, const char *sql)
{
   char err[TPL_ERRBUF] = "";
   void *c = aimee_pg_open(url, err, sizeof(err));
   if (!c)
   {
      fprintf(stderr, "db2-test-template: connect failed: %s\n", err);
      return -1;
   }
   int rc = aimee_pg_exec(c, sql, err, sizeof(err));
   if (rc != 0)
      fprintf(stderr, "db2-test-template: %s\n  failed: %s\n", sql, err);
   aimee_pg_close(c);
   return rc;
}

static char *slurp(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long n = ftell(f);
   if (n < 0 || fseek(f, 0, SEEK_SET) != 0)
   {
      fclose(f);
      return NULL;
   }
   char *buf = (char *)malloc((size_t)n + 1);
   if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n)
      buf[n] = '\0';
   else
   {
      free(buf);
      buf = NULL;
   }
   fclose(f);
   return buf;
}

int main(int argc, char **argv)
{
   if (argc != 3)
   {
      fprintf(stderr, "usage: %s <template-url> <reset-sql-path>\n", argv[0]);
      return 2;
   }
   const char *url = argv[1];
   const char *reset_path = argv[2];

   char prefix[1024], dbname[256], suffix[256];
   if (split_url(url, prefix, sizeof(prefix), dbname, sizeof(dbname), suffix, sizeof(suffix)) != 0)
   {
      fprintf(stderr, "db2-test-template: cannot parse url (%s)\n", url);
      return 2;
   }

   char *reset_sql = slurp(reset_path);
   if (!reset_sql)
   {
      fprintf(stderr, "db2-test-template: cannot read %s\n", reset_path);
      return 2;
   }

   char admin_url[1400];
   snprintf(admin_url, sizeof(admin_url), "%spostgres%s", prefix, suffix);

   /* Rebuild from scratch: a template carrying a previous run's leftovers would
    * hand every test binary a database that is not the freshly-seeded state the
    * reset restores back to. FORCE evicts clones still connected from a killed run. */
   /* AIMEE_TEMPLATE_KEEP applies the schema to the database as it stands.
    *
    * The default is to rebuild, and that is right for a template. It is exactly
    * wrong for the one question a template can never answer: whether a database
    * carrying an OLDER schema survives this one being applied over it. Dropping
    * first makes every teardown statement look like it worked, because there
    * was nothing to tear down. See
    * scripts/validation/db2-schema/upgrade-over-installed-schema.sh, which sets
    * this and found two defects that every from-scratch suite reported green. */
   char sql[768];
   if (!getenv("AIMEE_TEMPLATE_KEEP"))
   {
      snprintf(sql, sizeof(sql), "DROP DATABASE IF EXISTS \"%s\" WITH (FORCE)", dbname);
      if (admin_exec(admin_url, sql) != 0)
      {
         free(reset_sql);
         return 1;
      }
      snprintf(sql, sizeof(sql), "CREATE DATABASE \"%s\"", dbname);
      if (admin_exec(admin_url, sql) != 0)
      {
         free(reset_sql);
         return 1;
      }
   }

   /* db2_init applies the schema, including the rows schema.sql seeds. */
   db2_set_embedding_dim_default(CONFIG_EMBEDDER_DIMS_DEFAULT);
   db2_set_embedding_dim(CONFIG_EMBEDDER_DIMS_DEFAULT);
   if (db2_init(url) != 0)
   {
      fprintf(stderr, "db2-test-template: db2_init failed against %s\n", url);
      free(reset_sql);
      return 1;
    }

   char err[TPL_ERRBUF] = "";
   int rc = aimee_pg_exec(db2_conn(), reset_sql, err, sizeof(err));
   free(reset_sql);
   if (rc != 0)
   {
      fprintf(stderr, "db2-test-template: installing reset helpers failed: %s\n", err);
      db2_shutdown();
      return 1;
   }

   if (aimee_pg_exec(db2_conn(), "SELECT aimee_test_seed_capture()", err, sizeof(err)) != 0)
   {
      fprintf(stderr, "db2-test-template: seed capture failed: %s\n", err);
      db2_shutdown();
      return 1;
   }

   db2_shutdown();
   printf("db2-test-template: %s ready\n", dbname);
   return 0;
}
