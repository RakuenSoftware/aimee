/* db2/db2_hardening.c: hardened-tier boot assertions. See db2_hardening.h. */

#include "db2_hardening.h"
#include "db_postgres.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db2_hardening_enabled(void)
{
   const char *v = getenv("AIMEE_KB_HARDENED");
   return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
}

/* Extract the effective sslmode from a libpq URL or keyword/value DSN. Returns a
 * lowercased copy in out[cap]; empty string when unspecified. Handles both
 * "sslmode=..." (keyword DSN and URL query) forms. */
static void dsn_sslmode(const char *url, char *out, size_t cap)
{
   out[0] = '\0';
   if (!url)
      return;
   const char *p = url;
   while ((p = strstr(p, "sslmode")) != NULL)
   {
      const char *q = p + 7; /* past "sslmode" */
      while (*q == ' ')
         ++q;
      if (*q != '=')
      {
         p = q;
         continue;
      }
      ++q;
      while (*q == ' ' || *q == '\'')
         ++q;
      size_t n = 0;
      while (*q && *q != '&' && *q != ' ' && *q != '\'' && n + 1 < cap)
         out[n++] = (char)tolower((unsigned char)*q++);
      out[n] = '\0';
      return;
   }
}

int db2_hardening_dsn_verify_full(const char *libpq_url)
{
   char mode[32];
   dsn_sslmode(libpq_url, mode, sizeof(mode));
   return strcmp(mode, "verify-full") == 0;
}

int db2_hardening_assert_runtime_role(void *conn, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!conn)
   {
      snprintf(err, errlen, "no connection");
      return 1;
   }
   char e[256] = "";
   /* current_user privileges: bypassrls, superuser, and CREATE on public. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT r.rolbypassrls, r.rolsuper, "
       "has_schema_privilege(current_user,'public','CREATE') "
       "FROM pg_roles r WHERE r.rolname = current_user",
       e, sizeof(e));
   if (!st)
   {
      snprintf(err, errlen, "role introspection prepare failed: %s", e);
      return 2;
   }
   int rc = 0;
   if (aimee_pg_step(st, e, sizeof(e)) == AIMEE_PG_ROW)
   {
      int bypassrls = aimee_pg_column_int(st, 0);
      int super = aimee_pg_column_int(st, 1);
      int create_pub = aimee_pg_column_int(st, 2);
      if (bypassrls)
      {
         snprintf(err, errlen, "runtime role has BYPASSRLS (defeats tenant RLS)");
         rc = 3;
      }
      else if (super)
      {
         snprintf(err, errlen, "runtime role is a superuser (bypasses RLS)");
         rc = 4;
      }
      else if (create_pub)
      {
         snprintf(err, errlen, "runtime role holds CREATE on public (must be DML-only)");
         rc = 5;
      }
   }
   else
   {
      snprintf(err, errlen, "role introspection returned no row: %s", e);
      rc = 6;
   }
   aimee_pg_finalize(st);
   return rc;
}
