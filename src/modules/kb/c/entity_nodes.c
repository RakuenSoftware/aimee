/* src/modules/kb/c/entity_nodes.c: entity_nodes table + aliases — Postgres via libpq. */

#include "entity_nodes.h"
#include "kb_store_internal.h"
#include "db_postgres.h"

#include <openssl/sha.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define EN_ERRBUF 256

int kb_store_entity_node_encode_component(const char *in, char *out, size_t cap)
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

int kb_store_entity_node_key_file(const char *project, const char *path, char *out, size_t cap)
{
   if (!project || !path || !out || cap == 0)
      return -1;
   char ep[GRAPH_ENDPOINT_MAX], pp[GRAPH_ENDPOINT_MAX];
   kb_store_entity_node_encode_component(project, ep, sizeof(ep));
   kb_store_entity_node_encode_component(path, pp, sizeof(pp));
   char combined[GRAPH_ENDPOINT_MAX * 2];
   snprintf(combined, sizeof(combined), "%s:%s", ep, pp);
   return key_build("file", combined, out, cap);
}

int kb_store_entity_node_key_symbol(const char *project, const char *name, char *out, size_t cap)
{
   if (!project || !name || !out || cap == 0)
      return -1;
   char ep[GRAPH_ENDPOINT_MAX], en[GRAPH_ENDPOINT_MAX];
   kb_store_entity_node_encode_component(project, ep, sizeof(ep));
   kb_store_entity_node_encode_component(name, en, sizeof(en));
   char combined[GRAPH_ENDPOINT_MAX * 2];
   snprintf(combined, sizeof(combined), "%s:%s", ep, en);
   return key_build("symbol", combined, out, cap);
}

int kb_store_entity_node_key_concept(const char *token, char *out, size_t cap)
{
   if (!token || !out || cap == 0)
      return -1;
   char enc[GRAPH_ENDPOINT_MAX];
   kb_store_entity_node_encode_component(token, enc, sizeof(enc));
   return key_build("concept", enc, out, cap);
}

int kb_store_entity_node_key_project(const char *name, char *out, size_t cap)
{
   if (!name || !out || cap == 0)
      return -1;
   char enc[GRAPH_ENDPOINT_MAX];
   kb_store_entity_node_encode_component(name, enc, sizeof(enc));
   return key_build("project", enc, out, cap);
}

int kb_store_entity_node_get(const char *node_key, kb_store_entity_node_t *out)
{
   if (!node_key || !*node_key || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   void *conn = kb_store_conn();
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
