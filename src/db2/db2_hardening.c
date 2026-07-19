/* db2/db2_hardening.c: hardened-tier boot assertions. See db2_hardening.h. */

#include "db2_hardening.h"
#include "db_postgres.h"
#include "log.h"

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int db2_hardening_enabled(void)
{
   const char *v = getenv("AIMEE_KB_HARDENED");
   return v && (v[0] == '1' || v[0] == 't' || v[0] == 'T' || v[0] == 'y' || v[0] == 'Y');
}

int db2_hardening_dsn_verify_full(const char *libpq_url)
{
   if (!libpq_url || !libpq_url[0])
      return 0;
   /* Use libpq's own parser so we read the EFFECTIVE sslmode exactly as the driver
    * would — not a handwritten scan that a value like password='sslmode=...' or a
    * duplicate parameter could fool. PQconninfoParse handles URL and keyword/value
    * forms and last-wins duplicates. */
   char *errmsg = NULL;
   PQconninfoOption *opts = PQconninfoParse(libpq_url, &errmsg);
   if (!opts)
   {
      if (errmsg)
         PQfreemem(errmsg);
      return 0; /* unparseable DSN — fail closed */
   }
   int ok = 0;
   for (PQconninfoOption *o = opts; o->keyword; ++o)
   {
      if (strcmp(o->keyword, "sslmode") == 0)
      {
         ok = (o->val && strcmp(o->val, "verify-full") == 0);
         break;
      }
   }
   PQconninfoFree(opts);
   return ok;
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
   /* current_user privileges: bypassrls, superuser, CREATE on public, AND whether
    * current_user is a member (directly or transitively) of any superuser/
    * BYPASSRLS role — an inherited or SET ROLE-able privileged membership defeats
    * RLS just as a direct attribute would, so it is rejected too. */
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn,
       "SELECT r.rolbypassrls, r.rolsuper, "
       "has_schema_privilege(current_user,'public','CREATE'), "
       "COALESCE((SELECT bool_or(m.rolsuper OR m.rolbypassrls) FROM pg_roles m "
       " WHERE m.rolname <> current_user AND pg_has_role(current_user, m.oid, 'MEMBER')), false) "
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
      int priv_member = aimee_pg_column_int(st, 3);
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
      else if (priv_member)
      {
         snprintf(
             err, errlen,
             "runtime role is a member of a superuser/BYPASSRLS role (can SET ROLE to bypass RLS)");
         rc = 7;
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
