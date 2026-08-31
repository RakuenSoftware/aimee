/* src/modules/db2/c/entity_nodes.c: entity_nodes table + aliases — Postgres via libpq. */

#include "entity_nodes.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <openssl/sha.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define EN_ERRBUF 256

int db2_entity_node_encode_component(const char *in, char *out, size_t cap)
{
   if (!in || !out || cap == 0)
      return -1;
   const unsigned char *p = (const unsigned char *)in;
   char *w = out;
   while (*p && (size_t)(w - out) < cap - 1)
   {
      unsigned char c = *p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
          c == '_' || c == '-' || c == '~' || c == '/')
      {
         *w++ = (char)c;
      }
      else
      {
         if ((size_t)(w - out) + 3 >= cap)
            break;
         *w++ = '%';
         *w++ = "0123456789ABCDEF"[c >> 4];
         *w++ = "0123456789ABCDEF"[c & 0xf];
      }
      p++;
   }
   *w = '\0';
   return (int)(w - out);
}

/* SHA-256 hex: first 32 lowercase hex chars (16 bytes) of SHA-256(str). */
static void sha256_hex32(const char *str, char *out)
{
   unsigned char hash[SHA256_DIGEST_LENGTH];
   SHA256((const unsigned char *)str, strlen(str), hash);
   for (int i = 0; i < 16; i++)
      snprintf(out + i * 2, 3, "%02x", hash[i]);
   out[32] = '\0';
}

/* Build a canonical key: prefix:encoded.
 * If length exceeds cap-1, compact to prefix:h:<hex32>. */
static int key_build(const char *prefix, const char *encoded, char *out, size_t cap)
{
   if (!prefix || !encoded || !out || cap == 0)
      return -1;
   size_t needed = strlen(prefix) + 1 + strlen(encoded) + 1;
   if (needed <= cap)
   {
      snprintf(out, cap, "%s:%s", prefix, encoded);
      return 0;
   }
   /* Compact: prefix:h:<first32hex of SHA-256(full_key)> */
   char full[1024];
   snprintf(full, sizeof(full), "%s:%s", prefix, encoded);
   char hex[33];
   sha256_hex32(full, hex);
   needed = strlen(prefix) + 3 + 32 + 1; /* prefix + ":h:" + 32 + NUL */
   if (needed > cap)
      return -1;
   snprintf(out, cap, "%s:h:%s", prefix, hex);
   return 0;
}

int db2_entity_node_key_file(const char *project, const char *path, char *out, size_t cap)
{
   if (!project || !path || !out || cap == 0)
      return -1;
   char ep[GRAPH_ENDPOINT_MAX], pp[GRAPH_ENDPOINT_MAX];
   db2_entity_node_encode_component(project, ep, sizeof(ep));
   db2_entity_node_encode_component(path, pp, sizeof(pp));
   char combined[GRAPH_ENDPOINT_MAX * 2];
   snprintf(combined, sizeof(combined), "%s:%s", ep, pp);
   return key_build("file", combined, out, cap);
}

int db2_entity_node_key_symbol(const char *project, const char *name, char *out, size_t cap)
{
   if (!project || !name || !out || cap == 0)
      return -1;
   char ep[GRAPH_ENDPOINT_MAX], en[GRAPH_ENDPOINT_MAX];
   db2_entity_node_encode_component(project, ep, sizeof(ep));
   db2_entity_node_encode_component(name, en, sizeof(en));
   char combined[GRAPH_ENDPOINT_MAX * 2];
   snprintf(combined, sizeof(combined), "%s:%s", ep, en);
   return key_build("symbol", combined, out, cap);
}

int db2_entity_node_key_concept(const char *token, char *out, size_t cap)
{
   if (!token || !out || cap == 0)
      return -1;
   char enc[GRAPH_ENDPOINT_MAX];
   db2_entity_node_encode_component(token, enc, sizeof(enc));
   return key_build("concept", enc, out, cap);
}

int db2_entity_node_key_project(const char *name, char *out, size_t cap)
{
   if (!name || !out || cap == 0)
      return -1;
   char enc[GRAPH_ENDPOINT_MAX];
   db2_entity_node_encode_component(name, enc, sizeof(enc));
   return key_build("project", enc, out, cap);
}

int db2_entity_node_upsert(const char *node_key, int node_kind, const char *project,
                           const char *display_name, const char *full_key, const char *file_path,
                           const char *symbol, const char *node_origin, int64_t generation_id)
{
   if (!node_key || !*node_key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "INSERT INTO entity_nodes (node_key, node_kind, project, display_name,"
                            " full_key, file_path, symbol, node_origin, last_seen_generation_id,"
                            " updated_at)"
                            " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9,"
                            "  to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS'))"
                            " ON CONFLICT(node_key) DO UPDATE SET"
                            "  node_kind = excluded.node_kind,"
                            "  project = excluded.project,"
                            "  display_name = excluded.display_name,"
                            "  full_key = excluded.full_key,"
                            "  file_path = excluded.file_path,"
                            "  symbol = excluded.symbol,"
                            "  node_origin = excluded.node_origin,"
                            "  last_seen_generation_id = excluded.last_seen_generation_id,"
                            "  updated_at = to_char(CURRENT_TIMESTAMP, 'YYYY-MM-DD HH24:MI:SS')";
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", node_key);
   aimee_pg_bind_int(st, "?2", node_kind);
   aimee_pg_bind_text(st, "?3", project ? project : "");
   aimee_pg_bind_text(st, "?4", display_name ? display_name : node_key);
   aimee_pg_bind_text(st, "?5", full_key ? full_key : node_key);
   aimee_pg_bind_text(st, "?6", file_path ? file_path : "");
   aimee_pg_bind_text(st, "?7", symbol ? symbol : "");
   aimee_pg_bind_text(st, "?8", node_origin ? node_origin : "");
   aimee_pg_bind_int64(st, "?9", generation_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_entity_node_get(const char *node_key, db2_entity_node_t *out)
{
   if (!node_key || !*node_key || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "SELECT node_key, node_kind, project, display_name, full_key,"
                            " file_path, symbol, node_origin, last_seen_generation_id"
                            " FROM entity_nodes WHERE node_key = ?1";
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", node_key);
   int rc = -1;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v;
      v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out->node_key, sizeof(out->node_key), "%s", v);
      out->node_kind = aimee_pg_column_int(st, 1);
      v = aimee_pg_column_text(st, 2);
      if (v)
         snprintf(out->project, sizeof(out->project), "%s", v);
      v = aimee_pg_column_text(st, 3);
      if (v)
         snprintf(out->display_name, sizeof(out->display_name), "%s", v);
      v = aimee_pg_column_text(st, 4);
      if (v)
         snprintf(out->full_key, sizeof(out->full_key), "%s", v);
      v = aimee_pg_column_text(st, 5);
      if (v)
         snprintf(out->file_path, sizeof(out->file_path), "%s", v);
      v = aimee_pg_column_text(st, 6);
      if (v)
         snprintf(out->symbol, sizeof(out->symbol), "%s", v);
      v = aimee_pg_column_text(st, 7);
      if (v)
         snprintf(out->node_origin, sizeof(out->node_origin), "%s", v);
      out->last_seen_generation_id = aimee_pg_column_int64(st, 8);
      rc = 0;
   }
   aimee_pg_finalize(st);
   return rc;
}

int db2_entity_node_alias_upsert(const char *alias, const char *node_key, const char *alias_kind,
                                 const char *project, int64_t generation_id)
{
   if (!alias || !*alias || !node_key || !*node_key)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   static const char *sql = "INSERT INTO entity_node_aliases (alias, node_key, alias_kind, project,"
                            " last_seen_generation_id)"
                            " VALUES (?1, ?2, ?3, ?4, ?5)"
                            " ON CONFLICT(alias, node_key) DO UPDATE SET"
                            "  alias_kind = excluded.alias_kind,"
                            "  project = excluded.project,"
                            "  last_seen_generation_id = excluded.last_seen_generation_id";
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_text(st, "?1", alias);
   aimee_pg_bind_text(st, "?2", node_key);
   aimee_pg_bind_text(st, "?3", alias_kind ? alias_kind : "");
   aimee_pg_bind_text(st, "?4", project ? project : "");
   aimee_pg_bind_int64(st, "?5", generation_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? 0 : -1;
}

int db2_entity_node_resolve_alias(const char *alias, const char *project,
                                  char (*out_keys)[GRAPH_ENDPOINT_MAX], int max)
{
   if (!alias || !*alias || !out_keys || max <= 0)
      return 0;
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[512];
   if (project && *project)
   {
      snprintf(sql, sizeof(sql),
               "SELECT a.node_key FROM entity_node_aliases a"
               " JOIN entity_nodes n ON n.node_key = a.node_key"
               " WHERE a.alias = ?1 AND a.project = ?2"
               " ORDER BY n.last_seen_generation_id DESC LIMIT %d",
               max);
   }
   else
   {
      snprintf(sql, sizeof(sql),
               "SELECT node_key FROM entity_node_aliases"
               " WHERE alias = ?1"
               " ORDER BY last_seen_generation_id DESC LIMIT %d",
               max);
   }
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", alias);
   if (project && *project)
      aimee_pg_bind_text(st, "?2", project);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         snprintf(out_keys[n], GRAPH_ENDPOINT_MAX, "%s", v);
      n++;
   }
   aimee_pg_finalize(st);
   return n;
}

int db2_entity_node_cleanup_stale_code(int64_t min_generation_id)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   static const char *sql = "DELETE FROM entity_nodes"
                            " WHERE node_origin = 'code_projection'"
                            "   AND last_seen_generation_id < ?1";
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", min_generation_id);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}

int db2_entity_node_alias_cleanup_stale(const char *project, int64_t min_generation_id)
{
   void *conn = db2_conn();
   if (!conn)
      return 0;
   char sql[512];
   if (project && *project)
      snprintf(sql, sizeof(sql),
               "DELETE FROM entity_node_aliases"
               " WHERE last_seen_generation_id < ?1 AND project = ?2");
   else
      snprintf(sql, sizeof(sql),
               "DELETE FROM entity_node_aliases"
               " WHERE last_seen_generation_id < ?1");
   char err[EN_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, sql, err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_int64(st, "?1", min_generation_id);
   if (project && *project)
      aimee_pg_bind_text(st, "?2", project);
   int rc = aimee_pg_step(st, err, sizeof(err));
   int changes = aimee_pg_stmt_changes(st);
   aimee_pg_finalize(st);
   return (rc == AIMEE_PG_DONE) ? changes : 0;
}
