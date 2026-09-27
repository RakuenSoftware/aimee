/* db2/curator_terms.c: corpus terminology normalization.
 *
 * Stage 6: extract surface terms from doc normalized_text, match against
 * existing entities/term_mappings, write term_mapping artifacts for new
 * canonicalizations.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "curator_terms.h"
#include "artifacts.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CT_ERRBUF 256

/* Simple token collector: produces at most MAX_TOK candidate terms.
 * Groups adjacent uppercase/CamelCase tokens and quoted spans. */
#define MAX_TOK 50
typedef struct
{
   char terms[MAX_TOK][64];
   int count;
} term_cands_t;

static int tok_is_upper(int c)
{
   return c >= 'A' && c <= 'Z';
}
static int tok_is_lower(int c)
{
   return c >= 'a' && c <= 'z';
}
static int tok_is_digit(int c)
{
   return c >= '0' && c <= '9';
}

static int tok_len_ok(size_t len)
{
   return len >= 2 && len < 60;
}

static void tok_collect(const char *text, term_cands_t *out)
{
   memset(out, 0, sizeof(*out));
   if (!text || !text[0])
      return;
   const char *p = text;
   char buf[64] = "";
   size_t buflen = 0;
   enum
   {
      S_WS,
      S_WORD
   } state = S_WS;

   while (*p && out->count < MAX_TOK)
   {
      int c = (unsigned char)*p;
      if (c == '`' || c == '"' || c == '\'')
      {
         int quote = c;
         p++;
         buflen = 0;
         while (*p && *p != quote && buflen < sizeof(buf) - 1)
            buf[buflen++] = *p++;
         buf[buflen] = '\0';
         while (*p && *p != quote)
            p++;
         if (*p)
            p++;
         if (tok_len_ok(buflen))
         {
            strncpy(out->terms[out->count], buf, sizeof(out->terms[0]) - 1);
            out->terms[out->count][sizeof(out->terms[0]) - 1] = '\0';
            out->count++;
         }
         buflen = 0;
         state = S_WS;
         continue;
      }
      if (isspace(c) || ispunct(c))
      {
         if (state == S_WORD && tok_len_ok(buflen))
         {
            strncpy(out->terms[out->count], buf, sizeof(out->terms[0]) - 1);
            out->terms[out->count][sizeof(out->terms[0]) - 1] = '\0';
            out->count++;
         }
         buflen = 0;
         state = S_WS;
         p++;
         continue;
      }
      if (tok_is_lower(c) || tok_is_digit(c))
      {
         if (state == S_WORD && buflen < sizeof(buf) - 1)
            buf[buflen++] = (char)c;
         p++;
         continue;
      }
      if (tok_is_upper(c))
      {
         if (state == S_WORD)
         {
            int prev = (buflen > 0) ? (unsigned char)buf[buflen - 1] : 0;
            if (tok_is_lower(prev) || tok_is_digit(prev))
            {
               if (tok_len_ok(buflen) && out->count < MAX_TOK)
               {
                  strncpy(out->terms[out->count], buf, sizeof(out->terms[0]) - 1);
                  out->terms[out->count][sizeof(out->terms[0]) - 1] = '\0';
                  out->count++;
               }
               buflen = 0;
               state = S_WS;
            }
         }
         if (state != S_WORD)
         {
            buflen = 0;
            state = S_WORD;
         }
         if (buflen < sizeof(buf) - 1)
            buf[buflen++] = (char)c;
         p++;
         continue;
      }
      if (c == '_')
      {
         if (state == S_WORD && buflen < sizeof(buf) - 1)
            buf[buflen++] = (char)c;
         p++;
         continue;
      }
      p++;
   }
   if (state == S_WORD && tok_len_ok(buflen) && out->count < MAX_TOK)
   {
      strncpy(out->terms[out->count], buf, sizeof(out->terms[0]) - 1);
      out->terms[out->count][sizeof(out->terms[0]) - 1] = '\0';
      out->count++;
   }
}

/* Check if a term_mapping already exists for raw_term (case-insensitive text match). */
static int term_mapping_exists(const char *raw_term)
{
   void *conn = db2_conn();
   if (!conn || !raw_term || !raw_term[0])
      return 0;
   char err[CT_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn,
                                          "SELECT COUNT(*) FROM artifacts"
                                          " WHERE kind = 'term_mapping'"
                                          "   AND state <> 'retired'"
                                          "   AND payload::text ILIKE '%' || ?1 || '%'",
                                          err, sizeof(err));
   if (!st)
      return 0;
   aimee_pg_bind_text(st, "?1", raw_term);
   int exists = 0;
   if (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      exists = aimee_pg_column_int(st, 0) > 0;
   aimee_pg_finalize(st);
   return exists;
}

/* Canonicalize a surface term: lowercase, trim. */
static void canonical_form(const char *src, char *dst, size_t dst_len)
{
   if (!src || !src[0])
   {
      if (dst && dst_len > 0)
         dst[0] = '\0';
      return;
   }
   size_t j = 0;
   for (size_t i = 0; src[i] && j < dst_len - 1; i++)
   {
      int c = (unsigned char)src[i];
      if (!isspace(c))
         dst[j++] = (char)tolower(c);
   }
   dst[j] = '\0';
}

int db2_corpus_normalize_terms(int64_t doc_id)
{
   void *conn = db2_conn();
   if (!conn || doc_id <= 0)
      return -1;
   char err[CT_ERRBUF] = "";

   /* Load normalized_text from docs table. */
   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT normalized_text FROM docs WHERE id = ?1", err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int64(st, "?1", doc_id);
   if (aimee_pg_step(st, err, sizeof(err)) != AIMEE_PG_ROW)
   {
      aimee_pg_finalize(st);
      return -1;
   }
   const char *normalized_text = aimee_pg_column_text(st, 0);
   if (!normalized_text || !normalized_text[0])
   {
      aimee_pg_finalize(st);
      return 0;
   }
   char text_buf[16384];
   snprintf(text_buf, sizeof(text_buf), "%s", normalized_text);
   aimee_pg_finalize(st);

   term_cands_t cands;
   tok_collect(text_buf, &cands);
   if (cands.count == 0)
      return 0;

   int written = 0;
   char doc_id_str[32];
   snprintf(doc_id_str, sizeof(doc_id_str), "%lld", (long long)doc_id);

   for (int i = 0; i < cands.count && written < MAX_TOK; i++)
   {
      const char *raw = cands.terms[i];
      if (term_mapping_exists(raw))
         continue;

      char artifact_id[37];
      db2_artifact_gen_id(artifact_id, sizeof(artifact_id));

      char preferred[128];
      canonical_form(raw, preferred, sizeof(preferred));

      char payload[512];
      snprintf(payload, sizeof(payload),
               "{\"raw_term\":\"%s\",\"preferred_term\":\"%s\","
               "\"term_kind\":\"alias\",\"scope_note\":\"auto-detected\"}",
               raw, preferred);

      if (db2_artifact_write(artifact_id, "term_mapping", "proposed", "global", "global",
                             "corpus.terms", 0.7, payload) == 0)
      {
         db2_artifact_cite(artifact_id, "doc", doc_id_str);
         written++;
      }
   }

   return written;
}
