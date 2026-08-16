/* db2/tool_registry.c: tool registry — Postgres via libpq. */

#include "tool_registry.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <stdio.h>
#include <string.h>

int db2_tool_registry_lookup(const char *name, tool_registry_entry_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   void *conn = db2_conn();
   if (!conn || !name)
      return -1;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT input_schema, side_effect, enabled FROM tool_registry WHERE name = ?1", err,
       sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", name);

   int rc = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *schema = aimee_pg_column_text(st, 0);
      const char *se = aimee_pg_column_text(st, 1);
      snprintf(out->input_schema, sizeof(out->input_schema), "%s", schema ? schema : "");
      snprintf(out->side_effect, sizeof(out->side_effect), "%s", se ? se : "read");
      out->enabled = aimee_pg_column_int(st, 2);
      out->found = 1;
   }
   aimee_pg_finalize(st);
   return rc;
}

const char *db2_tool_registry_side_effect(const char *name)
{
   static __thread char buf[32];
   buf[0] = '\0';

   void *conn = db2_conn();
   if (!conn || !name)
      return "read";

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT side_effect FROM tool_registry WHERE name = ?1", err, sizeof(err));
   if (!st)
      return "read";
   aimee_pg_bind_text(st, "?1", name);

   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *val = aimee_pg_column_text(st, 0);
      snprintf(buf, sizeof(buf), "%s", val ? val : "read");
   }
   aimee_pg_finalize(st);
   return buf[0] ? buf : "read";
}

int db2_tool_registry_iter_prompts(tool_registry_prompt_cb cb, void *user)
{
   if (!cb)
      return 0;

   void *conn = db2_conn();
   if (!conn)
      return 0;

   char err[256] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(
       conn, "SELECT name, tool_prompt FROM tool_registry WHERE enabled = 1 ORDER BY name", err,
       sizeof(err));
   if (!st)
      return 0;

   int rc = 0;
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *name = aimee_pg_column_text(st, 0);
      const char *prompt = aimee_pg_column_text(st, 1);
      if (!name)
         continue;
      rc = cb(name, prompt ? prompt : "", user);
      if (rc != 0)
         break;
   }
   aimee_pg_finalize(st);
   return rc;
}
