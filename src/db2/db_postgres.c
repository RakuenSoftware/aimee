/* libpq-backed implementation for DB2, the shared knowledge tier. See
 * docs/STORAGE_TIERS.md. */

#include "db_postgres.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --- SQL placeholder rewriter ------------------------------------ */

static int is_name_start(int c)
{
   return isalpha(c) || c == '_';
}
static int is_name_cont(int c)
{
   return isalnum(c) || c == '_';
}

static int buf_append(char **buf, size_t *cap, size_t *len, const char *src, size_t n)
{
   if (*len + n + 1 > *cap)
   {
      size_t ncap = (*cap) * 2;
      while (*len + n + 1 > ncap)
         ncap *= 2;
      char *tmp = realloc(*buf, ncap);
      if (!tmp)
         return -1;
      *buf = tmp;
      *cap = ncap;
   }
   memcpy(*buf + *len, src, n);
   *len += n;
   (*buf)[*len] = '\0';
   return 0;
}

static int append_param_name(char ***names, int *nn, int *nn_cap, const char *name)
{
   int idx = -1;
   for (int i = 0; i < *nn; i++)
   {
      if (strcmp((*names)[i], name) == 0)
      {
         idx = i;
         break;
      }
   }
   if (idx >= 0)
      return idx;

   if (*nn == *nn_cap)
   {
      int ncap = *nn_cap ? (*nn_cap) * 2 : 8;
      char **tmp = realloc(*names, (size_t)ncap * sizeof(char *));
      if (!tmp)
         return -1;
      *names = tmp;
      *nn_cap = ncap;
   }
   (*names)[*nn] = strdup(name);
   if (!(*names)[*nn])
      return -1;
   idx = (*nn)++;
   return idx;
}

void aimee_pg_free_names(char **names, int count)
{
   if (!names)
      return;
   for (int i = 0; i < count; i++)
      free(names[i]);
   free(names);
}

/* --- DB2 text-search rewriter ------------------------------------
 *
 * DB2 exposes `<X>_fts` views backed by generated tsvector columns + GIN
 * indexes. This rewriter translates the MATCH / bm25 query shape callers emit
 * into DB2 text-search predicates:
 *
 *   X_fts MATCH <expr>               → X_fts.fts_tsv @@ plainto_tsquery('simple', <expr>)
 *   X_fts.<col> MATCH <expr>         → X_fts.fts_tsv @@ plainto_tsquery('simple', <expr>)
 *   bm25(X_fts[, w1, w2, ...])       → (-ts_rank_cd(X_fts.fts_tsv, plainto_tsquery('simple',
 * <match_expr_for_X_fts>))) INSERT INTO X_fts (...)          → SELECT 1 WHERE false (DB2
 * tsvector cols are generated, so mirror maintenance inserts are no-ops; the view isn't
 * writable without INSTEAD OF triggers we don't ship.)
 *
 * Search features that don't translate:
 *   - Custom column weights in bm25(X, w1, w2, ...) degrade to the
 *     default ts_rank_cd weights {0.1, 0.2, 0.4, 1.0}.
 *   - Column-qualified MATCH (X.col MATCH ?) collapses to the whole
 *     tsvector; columns aren't indexed separately.
 *   - Advanced expression syntax (NEAR, column filters, ^) is
 *     passed through plainto_tsquery which interprets it as plain text.
 *
 * These deltas are covered by the DB2 retrieval parity budget.
 */

static int is_fts_ident(const char *start, const char *end)
{
   if (end - start < 5) /* at least "x_fts" */
      return 0;
   return strncasecmp(end - 4, "_fts", 4) == 0;
}

/* Text-search views that use trigram matching expose a generated
 * `<name>_text` column as `match_text`. The rewriter emits ILIKE and pg_trgm
 * `similarity()` against this column rather than @@ / ts_rank_cd. See the
 * DB2 text-search block in src/db2/schema.sql. */
static const char *const fts_trigram_tables[] = {
    "memories_code_fts",
};

static int is_trigram_fts(const char *start, const char *end)
{
   for (size_t i = 0; i < sizeof(fts_trigram_tables) / sizeof(fts_trigram_tables[0]); i++)
   {
      size_t n = strlen(fts_trigram_tables[i]);
      if ((size_t)(end - start) == n && strncasecmp(start, fts_trigram_tables[i], n) == 0)
         return 1;
   }
   return 0;
}

/* Skip past the argument of a MATCH clause: either a simple placeholder
 * (?, ?1, :name, @name, $N) or a single-quoted string literal. Returns
 * the position after the argument on success, or NULL if we can't
 * recognize the shape (caller should fall back). */
static const char *skip_match_arg(const char *q)
{
   if (!q)
      return NULL;
   if (*q == '?' || *q == ':' || *q == '@' || *q == '$')
   {
      q++;
      while (*q && (isalnum((unsigned char)*q) || *q == '_'))
         q++;
      return q;
   }
   if (*q == '\'')
   {
      q++;
      while (*q)
      {
         if (q[0] == '\'' && q[1] == '\'')
         {
            q += 2;
            continue;
         }
         if (*q++ == '\'')
            return q;
      }
      return NULL;
   }
   return NULL;
}

/* Scan forward for the first `... MATCH <arg>` clause anywhere in
 * haystack and capture the argument's byte range. Used to retrieve the
 * tsquery expression that bm25(...) should rank against. Assumes one
 * MATCH per query — the common aimee call-site shape. Returns 1 on
 * success, 0 if no MATCH / unrecognisable arg. */
static int find_any_match_arg(const char *haystack, const char **match_start,
                              const char **match_end)
{
   const char *p = haystack;
   while (*p)
   {
      if ((*p == 'M' || *p == 'm') && strncasecmp(p, "MATCH", 5) == 0 &&
          (p == haystack || !isalnum((unsigned char)p[-1])) &&
          (p[5] == ' ' || p[5] == '\t' || p[5] == '\n' || p[5] == '('))
      {
         const char *q = p + 5;
         while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '(')
            q++;
         const char *a_start = q;
         const char *a_end = skip_match_arg(q);
         if (a_end && a_end > a_start)
         {
            *match_start = a_start;
            *match_end = a_end;
            return 1;
         }
      }
      p++;
   }
   return 0;
}

static char *rewrite_db2_text_search(const char *sql)
{
   if (!sql)
      return NULL;

   /* Stage 1: INSERT INTO <X>_fts ... → no-op that still consumes the
    * original placeholders so callers binding by name (aimee_pg_bind_*)
    * don't hit unknown-param errors. We wrap the VALUES body in a
    * filtered-empty subquery: the placeholders are parsed and bound
    * but the row never materialises. If the INSERT isn't the simple
    * VALUES shape aimee uses everywhere (e.g. legacy rebuild control:
    * `INSERT INTO X_fts(X_fts) VALUES('rebuild')`), fall back to a
    * plain `SELECT 1 WHERE false`; those call sites don't bind. */
   const char *p = sql;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   if (strncasecmp(p, "INSERT INTO", 11) == 0 && (p[11] == ' ' || p[11] == '\t' || p[11] == '\n'))
   {
      const char *q = p + 11;
      while (*q == ' ' || *q == '\t' || *q == '\n')
         q++;
      const char *id_start = q;
      while (*q && (isalnum((unsigned char)*q) || *q == '_'))
         q++;
      const char *id_end = q;
      if (is_fts_ident(id_start, id_end))
      {
         const char *values = NULL;
         {
            const char *scan = q;
            while (*scan)
            {
               if ((scan[0] == 'V' || scan[0] == 'v') && strncasecmp(scan, "VALUES", 6) == 0 &&
                   (scan == sql || !isalnum((unsigned char)scan[-1])) &&
                   (scan[6] == ' ' || scan[6] == '\t' || scan[6] == '\n' || scan[6] == '('))
               {
                  values = scan;
                  break;
               }
               scan++;
            }
         }
         /* Check the VALUES body contains a placeholder — if it's an
          * rebuild-control string like VALUES('rebuild') there's nothing
          * to consume and the plain SELECT is sufficient. */
         int has_placeholder = 0;
         if (values)
         {
            for (const char *v = values; *v; v++)
            {
               if (*v == '\'')
               {
                  v++;
                  while (*v)
                  {
                     if (v[0] == '\'' && v[1] == '\'')
                     {
                        v += 2;
                        continue;
                     }
                     if (*v++ == '\'')
                        break;
                  }
                  if (!*v)
                     break;
                  continue;
               }
               if (*v == '?' || (*v == ':' && is_name_start((unsigned char)v[1])))
               {
                  has_placeholder = 1;
                  break;
               }
            }
         }
         if (values && has_placeholder)
         {
            /* Find the VALUES body's closing paren and strip the trailing
             * semicolon / whitespace. We need the parenthesised list so
             * we can inline it as a row-source. */
            const char *open = strchr(values, '(');
            if (open)
            {
               int depth = 0;
               const char *close = NULL;
               for (const char *v = open; *v; v++)
               {
                  if (*v == '(')
                     depth++;
                  else if (*v == ')')
                  {
                     depth--;
                     if (depth == 0)
                     {
                        close = v;
                        break;
                     }
                  }
               }
               if (close)
               {
                  size_t vlen = (size_t)(close - open + 1);
                  size_t total = vlen + 64;
                  char *buf = malloc(total);
                  if (buf)
                  {
                     size_t n = (size_t)snprintf(buf, total, "SELECT 1 FROM (VALUES ");
                     memcpy(buf + n, open, vlen);
                     n += vlen;
                     n += (size_t)snprintf(buf + n, total - n, ") AS _fts_noop WHERE false");
                     return buf;
                  }
               }
            }
         }
         return strdup("SELECT 1 WHERE false");
      }
   }

   /* Stage 2: scan for bm25(...) and MATCH clauses, build a new string
    * with translations. This is a linear scan — we don't attempt to
    * understand SQL structure beyond spotting the two token patterns. */
   if (!strstr(sql, "MATCH") && !strstr(sql, "match") && !strstr(sql, "bm25(") &&
       !strstr(sql, "BM25("))
      return NULL; /* nothing to do; caller uses original pointer */

   size_t in_len = strlen(sql);
   size_t cap = in_len * 2 + 256;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   size_t olen = 0;
#define FTS_APPEND(src, n)                                                                         \
   do                                                                                              \
   {                                                                                               \
      if (olen + (n) + 1 > cap)                                                                    \
      {                                                                                            \
         while (olen + (n) + 1 > cap)                                                              \
            cap *= 2;                                                                              \
         char *tmp = realloc(out, cap);                                                            \
         if (!tmp)                                                                                 \
         {                                                                                         \
            free(out);                                                                             \
            return NULL;                                                                           \
         }                                                                                         \
         out = tmp;                                                                                \
      }                                                                                            \
      memcpy(out + olen, (src), (n));                                                              \
      olen += (n);                                                                                 \
      out[olen] = '\0';                                                                            \
   } while (0)

   const char *cur = sql;
   while (*cur)
   {
      /* Pass literals and comments through verbatim so quoted MATCH /
       * bm25 substrings in strings or comments aren't mangled. */
      if (*cur == '\'')
      {
         const char *s = cur;
         cur++;
         while (*cur)
         {
            if (cur[0] == '\'' && cur[1] == '\'')
            {
               cur += 2;
               continue;
            }
            if (*cur++ == '\'')
               break;
         }
         FTS_APPEND(s, (size_t)(cur - s));
         continue;
      }
      if (*cur == '"')
      {
         const char *s = cur;
         cur++;
         while (*cur && *cur != '"')
            cur++;
         if (*cur == '"')
            cur++;
         FTS_APPEND(s, (size_t)(cur - s));
         continue;
      }
      if (cur[0] == '-' && cur[1] == '-')
      {
         const char *s = cur;
         while (*cur && *cur != '\n')
            cur++;
         FTS_APPEND(s, (size_t)(cur - s));
         continue;
      }
      if (cur[0] == '/' && cur[1] == '*')
      {
         const char *s = cur;
         cur += 2;
         while (*cur && !(cur[0] == '*' && cur[1] == '/'))
            cur++;
         if (*cur)
            cur += 2;
         FTS_APPEND(s, (size_t)(cur - s));
         continue;
      }

      /* bm25(<ident>[, ...]) */
      if ((cur[0] == 'b' || cur[0] == 'B') && strncasecmp(cur, "bm25(", 5) == 0)
      {
         const char *open = cur + 5;
         const char *q = open;
         while (*q == ' ' || *q == '\t' || *q == '\n')
            q++;
         const char *id_start = q;
         while (*q && (isalnum((unsigned char)*q) || *q == '_'))
            q++;
         const char *id_end = q;
         /* Find the matching close paren. */
         const char *close = q;
         while (*close && *close != ')')
            close++;
         if (*close == ')' && id_end > id_start && is_fts_ident(id_start, id_end))
         {
            const char *mstart = NULL, *mend = NULL;
            if (find_any_match_arg(close + 1, &mstart, &mend))
            {
               if (is_trigram_fts(id_start, id_end))
               {
                  FTS_APPEND("(-similarity(", 13);
                  FTS_APPEND(id_start, (size_t)(id_end - id_start));
                  FTS_APPEND(".match_text, ", 13);
                  FTS_APPEND(mstart, (size_t)(mend - mstart));
                  FTS_APPEND("))", 2);
               }
               else
               {
                  FTS_APPEND("(-ts_rank_cd(", 13);
                  FTS_APPEND(id_start, (size_t)(id_end - id_start));
                  FTS_APPEND(".fts_tsv, plainto_tsquery('simple', ", 36);
                  FTS_APPEND(mstart, (size_t)(mend - mstart));
                  FTS_APPEND(")))", 3);
               }
               cur = close + 1;
               continue;
            }
         }
         FTS_APPEND(cur, 5);
         cur += 5;
         continue;
      }

      /* <ident>[.<col>] MATCH <expr>
       *
       * MATCH implies the left-hand side is a text-search table/view
       * (possibly aliased) or a column on one. We rewrite to @@ fts_tsv
       * unconditionally; the table alias on the left is preserved so JOIN
       * column references keep resolving. The optional .col is discarded
       * because the tsvector is built over all search columns. */
      if ((isalpha((unsigned char)*cur) || *cur == '_') &&
          (cur == sql || !isalnum((unsigned char)cur[-1])))
      {
         const char *id_start = cur;
         const char *q = cur;
         while (*q && (isalnum((unsigned char)*q) || *q == '_'))
            q++;
         const char *id_end = q;
         const char *scan = q;
         if (*scan == '.')
         {
            scan++;
            while (*scan && (isalnum((unsigned char)*scan) || *scan == '_'))
               scan++;
         }
         const char *tail = scan;
         while (*tail == ' ' || *tail == '\t' || *tail == '\n')
            tail++;
         if (strncasecmp(tail, "MATCH", 5) == 0 &&
             (tail[5] == ' ' || tail[5] == '\t' || tail[5] == '\n' || tail[5] == '('))
         {
            const char *m = tail + 5;
            while (*m == ' ' || *m == '\t' || *m == '\n' || *m == '(')
               m++;
            const char *a_start = m;
            const char *a_end = skip_match_arg(m);
            if (a_end && a_end > a_start)
            {
               if (is_trigram_fts(id_start, id_end))
               {
                  FTS_APPEND(id_start, (size_t)(id_end - id_start));
                  FTS_APPEND(".match_text ILIKE '%' || ", 25);
                  FTS_APPEND(a_start, (size_t)(a_end - a_start));
                  FTS_APPEND(" || '%'", 7);
               }
               else
               {
                  FTS_APPEND(id_start, (size_t)(id_end - id_start));
                  FTS_APPEND(".fts_tsv @@ plainto_tsquery('simple', ", 38);
                  FTS_APPEND(a_start, (size_t)(a_end - a_start));
                  FTS_APPEND(")", 1);
               }
               cur = a_end;
               continue;
            }
         }
      }

      FTS_APPEND(cur, 1);
      cur++;
   }
#undef FTS_APPEND

   return out;
}

/* Normalise bare `?` positional placeholders to `?N` numbered form so
 * downstream passes (notably the FTS rewriter, which duplicates the
 * MATCH argument into bm25's ts_rank_cd call) can emit the same
 * placeholder in multiple positions and have the final placeholder
 * rewriter dedupe them to one $N. Without this, two textual `?`s in
 * the output get renumbered sequentially and point at different
 * parameters. Leaves `?N`, `:name`, and placeholders inside string
 * literals / comments alone. */
static char *prenumber_positional_placeholders(const char *sql)
{
   if (!sql || !strchr(sql, '?'))
      return NULL;

   size_t in_len = strlen(sql);
   size_t cap = in_len * 2 + 64;
   char *out = malloc(cap);
   if (!out)
      return NULL;
   size_t olen = 0;
   int next_num = 1;

#define PRE_APPEND(src, n)                                                                         \
   do                                                                                              \
   {                                                                                               \
      if (olen + (n) + 1 > cap)                                                                    \
      {                                                                                            \
         while (olen + (n) + 1 > cap)                                                              \
            cap *= 2;                                                                              \
         char *tmp = realloc(out, cap);                                                            \
         if (!tmp)                                                                                 \
         {                                                                                         \
            free(out);                                                                             \
            return NULL;                                                                           \
         }                                                                                         \
         out = tmp;                                                                                \
      }                                                                                            \
      memcpy(out + olen, (src), (n));                                                              \
      olen += (n);                                                                                 \
      out[olen] = '\0';                                                                            \
   } while (0)

   const char *p = sql;
   int touched = 0;
   while (*p)
   {
      if (*p == '\'')
      {
         const char *s = p;
         p++;
         while (*p)
         {
            if (p[0] == '\'' && p[1] == '\'')
            {
               p += 2;
               continue;
            }
            if (*p++ == '\'')
               break;
         }
         PRE_APPEND(s, (size_t)(p - s));
         continue;
      }
      if (*p == '"')
      {
         const char *s = p;
         p++;
         while (*p && *p != '"')
            p++;
         if (*p == '"')
            p++;
         PRE_APPEND(s, (size_t)(p - s));
         continue;
      }
      if (p[0] == '-' && p[1] == '-')
      {
         const char *s = p;
         while (*p && *p != '\n')
            p++;
         PRE_APPEND(s, (size_t)(p - s));
         continue;
      }
      if (p[0] == '/' && p[1] == '*')
      {
         const char *s = p;
         p += 2;
         while (*p && !(p[0] == '*' && p[1] == '/'))
            p++;
         if (*p)
            p += 2;
         PRE_APPEND(s, (size_t)(p - s));
         continue;
      }
      if (*p == '?')
      {
         /* Leave `?N` (already numbered) alone; number `?` followed by
          * anything non-digit. */
         if (isdigit((unsigned char)p[1]))
         {
            const char *s = p;
            p++;
            while (isdigit((unsigned char)*p))
               p++;
            PRE_APPEND(s, (size_t)(p - s));
            continue;
         }
         char buf[32];
         int n = snprintf(buf, sizeof(buf), "?%d", next_num++);
         PRE_APPEND(buf, (size_t)n);
         p++;
         touched = 1;
         continue;
      }
      PRE_APPEND(p, 1);
      p++;
   }
#undef PRE_APPEND
   if (!touched)
   {
      free(out);
      return NULL; /* no rewrite needed */
   }
   return out;
}

int aimee_pg_rewrite_params(const char *sql_in, char **out_sql, char ***out_names, int *out_count,
                            char *errbuf, size_t errlen)
{
   if (!sql_in || !out_sql || !out_names || !out_count)
      return -1;

   /* Step 1a: number bare positional `?` placeholders so the FTS
    * rewriter can duplicate them (MATCH arg reused by bm25) and still
    * dedupe to one $N downstream. */
   char *xlated = prenumber_positional_placeholders(sql_in);
   if (xlated)
      sql_in = xlated;

   /* Step 1b: translate text-search operators (MATCH, bm25, INSERT INTO
    * *_fts) into DB2 tsvector equivalents. */
   char *fts_xlated = rewrite_db2_text_search(sql_in);
   if (fts_xlated)
   {
      free(xlated);
      xlated = fts_xlated;
      sql_in = fts_xlated;
   }

   size_t cap = strlen(sql_in) + 64;
   char *out = malloc(cap);
   if (!out)
   {
      free(xlated);
      return -1;
   }
   out[0] = '\0';
   size_t olen = 0;

   char **names = NULL;
   int nn = 0, nn_cap = 0;

   const char *p = sql_in;
   while (*p)
   {
      /* Single-quoted string literal: copy verbatim, honoring the
       * SQL '' escape for embedded quotes. */
      if (*p == '\'')
      {
         buf_append(&out, &cap, &olen, p, 1);
         p++;
         while (*p)
         {
            if (p[0] == '\'' && p[1] == '\'')
            {
               buf_append(&out, &cap, &olen, p, 2);
               p += 2;
               continue;
            }
            buf_append(&out, &cap, &olen, p, 1);
            if (*p++ == '\'')
               break;
         }
         continue;
      }

      /* Double-quoted identifier: copy verbatim. */
      if (*p == '"')
      {
         buf_append(&out, &cap, &olen, p, 1);
         p++;
         while (*p && *p != '"')
         {
            buf_append(&out, &cap, &olen, p, 1);
            p++;
         }
         if (*p)
         {
            buf_append(&out, &cap, &olen, p, 1);
            p++;
         }
         continue;
      }

      /* Line comment -- … \n */
      if (p[0] == '-' && p[1] == '-')
      {
         while (*p && *p != '\n')
         {
            buf_append(&out, &cap, &olen, p, 1);
            p++;
         }
         continue;
      }

      /* SQL block comment: open and close sequences use slash-star / star-slash. */
      if (p[0] == '/' && p[1] == '*')
      {
         buf_append(&out, &cap, &olen, p, 2);
         p += 2;
         while (*p && !(p[0] == '*' && p[1] == '/'))
         {
            buf_append(&out, &cap, &olen, p, 1);
            p++;
         }
         if (*p)
         {
            buf_append(&out, &cap, &olen, p, 2);
            p += 2;
         }
         continue;
      }

      /* Postgres :: cast operator — pass through. */
      if (p[0] == ':' && p[1] == ':')
      {
         buf_append(&out, &cap, &olen, p, 2);
         p += 2;
         continue;
      }

      /* :name placeholder. */
      if (*p == ':' && is_name_start((unsigned char)p[1]))
      {
         p++;
         const char *start = p;
         while (is_name_cont((unsigned char)*p))
            p++;
         size_t len = (size_t)(p - start);
         if (len == 0 || len >= 128)
         {
            if (errbuf && errlen)
               snprintf(errbuf, errlen, "invalid placeholder name length %zu", len);
            free(out);
            aimee_pg_free_names(names, nn);
            free(xlated);
            return -1;
         }
         char name[128];
         memcpy(name, start, len);
         name[len] = '\0';

         int idx = append_param_name(&names, &nn, &nn_cap, name);
         if (idx < 0)
         {
            free(out);
            aimee_pg_free_names(names, nn);
            free(xlated);
            return -1;
         }

         char buf[24];
         int nprinted = snprintf(buf, sizeof(buf), "$%d", idx + 1);
         buf_append(&out, &cap, &olen, buf, (size_t)nprinted);
         continue;
      }

      /* DB2 positional placeholder: either a bare `?` numbered by appearance
       * or a `?N` form emitted by the pre-pass so repeated references point at
       * the same $N. */
      if (*p == '?')
      {
         p++;
         char name[32];
         if (isdigit((unsigned char)*p))
         {
            const char *num_start = p;
            while (isdigit((unsigned char)*p))
               p++;
            size_t len = (size_t)(p - num_start);
            if (len > 0 && len < sizeof(name) - 2)
               snprintf(name, sizeof(name), "?%.*s", (int)len, num_start);
            else
               snprintf(name, sizeof(name), "?%d", nn + 1);
         }
         else
         {
            snprintf(name, sizeof(name), "?%d", nn + 1);
         }
         int idx = append_param_name(&names, &nn, &nn_cap, name);
         if (idx < 0)
         {
            free(out);
            aimee_pg_free_names(names, nn);
            free(xlated);
            return -1;
         }
         char buf[24];
         int nprinted = snprintf(buf, sizeof(buf), "$%d", idx + 1);
         buf_append(&out, &cap, &olen, buf, (size_t)nprinted);
         continue;
      }

      buf_append(&out, &cap, &olen, p, 1);
      p++;
   }

   *out_sql = out;
   *out_names = names;
   *out_count = nn;
   free(xlated);
   return 0;
}

/* --- libpq-dependent code below ---------------------------------- */

#ifdef AIMEE_DISABLE_POSTGRES

/* Build-time opt-out for platforms where libpq isn't available (mostly
 * the Windows MinGW CI job — see the AIMEE_HAVE_LIBPQ block in
 * CMakeLists.txt). The SQL rewriter above stays live so the code
 * stays testable; all libpq entry points become stubs that fail
 * cleanly with a "backend unavailable" errmsg. */

#include "db_postgres.h"

void *aimee_pg_open(const char *c, char *e, size_t n)
{
   (void)c;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return NULL;
}
void aimee_pg_close(void *c)
{
   (void)c;
}
int aimee_pg_is_shim(void)
{
   return 0;
}
int aimee_pg_exec(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return -1;
}
int aimee_pg_exec_with_changes(void *c, const char *s, char *e, size_t n, int *a)
{
   (void)c;
   (void)s;
   (void)a;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return -1;
}
int aimee_pg_ping(void *c, char *e, size_t n)
{
   (void)c;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return -1;
}
aimee_pg_stmt_t *aimee_pg_prepare(void *c, const char *s, char *e, size_t n)
{
   (void)c;
   (void)s;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return NULL;
}
void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   (void)s;
}
int aimee_pg_reset(aimee_pg_stmt_t *s)
{
   (void)s;
   return -1;
}
int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *n, int v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *n, int64_t v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_double(aimee_pg_stmt_t *s, const char *n, double v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *n, const char *v)
{
   (void)s;
   (void)n;
   (void)v;
   return -1;
}
int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *n, const void *v, int l)
{
   (void)s;
   (void)n;
   (void)v;
   (void)l;
   return -1;
}
int aimee_pg_bind_null(aimee_pg_stmt_t *s, const char *n)
{
   (void)s;
   (void)n;
   return -1;
}
int aimee_pg_stmt_changes(aimee_pg_stmt_t *s)
{
   (void)s;
   return 0;
}
aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *e, size_t n)
{
   (void)s;
   if (e && n)
      snprintf(e, n, "libpq not linked");
   return AIMEE_PG_ERR;
}
int aimee_pg_column_count(aimee_pg_stmt_t *s)
{
   (void)s;
   return 0;
}
const char *aimee_pg_column_name(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
int aimee_pg_column_type(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return AIMEE_PG_VALUE_NULL;
}
const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 1;
}
const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return NULL;
}
int aimee_pg_column_int(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0;
}
double aimee_pg_column_double(aimee_pg_stmt_t *s, int c)
{
   (void)s;
   (void)c;
   return 0.0;
}

#else /* !AIMEE_DISABLE_POSTGRES */

#include <libpq-fe.h>

#define PG_TYPE_BOOL        16
#define PG_TYPE_BYTEA       17
#define PG_TYPE_INT8        20
#define PG_TYPE_INT2        21
#define PG_TYPE_INT4        23
#define PG_TYPE_TEXT        25
#define PG_TYPE_OID         26
#define PG_TYPE_FLOAT4      700
#define PG_TYPE_FLOAT8      701
#define PG_TYPE_BPCHAR      1042
#define PG_TYPE_VARCHAR     1043
#define PG_TYPE_DATE        1082
#define PG_TYPE_TIME        1083
#define PG_TYPE_TIMETZ      1266
#define PG_TYPE_TIMESTAMP   1114
#define PG_TYPE_TIMESTAMPTZ 1184
#define PG_TYPE_NUMERIC     1700
#define PG_TYPE_JSON        114
#define PG_TYPE_JSONB       3802

static void copy_err(char *errbuf, size_t errlen, const char *src)
{
   if (!errbuf || errlen == 0)
      return;
   snprintf(errbuf, errlen, "%s", src ? src : "");
}

void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen)
{
   PGconn *conn = PQconnectdb(conninfo ? conninfo : "");
   if (!conn)
   {
      copy_err(errbuf, errlen, "PQconnectdb returned NULL");
      return NULL;
   }
   if (PQstatus(conn) != CONNECTION_OK)
   {
      copy_err(errbuf, errlen, PQerrorMessage(conn));
      PQfinish(conn);
      return NULL;
   }
   return conn;
}

void aimee_pg_close(void *pg_conn)
{
   if (!pg_conn)
      return;
   PQfinish((PGconn *)pg_conn);
}

int aimee_pg_is_shim(void)
{
   return 0;
}

int aimee_pg_exec(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   return aimee_pg_exec_with_changes(pg_conn, sql, errbuf, errlen, NULL);
}

int aimee_pg_in_transaction(void *pg_conn)
{
   if (!pg_conn)
      return 0;
   PGTransactionStatusType t = PQtransactionStatus((PGconn *)pg_conn);
   return (t == PQTRANS_INTRANS || t == PQTRANS_INERROR) ? 1 : 0;
}

int aimee_pg_exec_with_changes(void *pg_conn, const char *sql, char *errbuf, size_t errlen,
                               int *affected_out)
{
   if (!pg_conn || !sql)
      return -1;
   PGresult *res = PQexec((PGconn *)pg_conn, sql);
   ExecStatusType st = PQresultStatus(res);
   if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
   {
      copy_err(errbuf, errlen, PQerrorMessage((PGconn *)pg_conn));
      PQclear(res);
      return -1;
   }
   if (affected_out)
   {
      const char *n = PQcmdTuples(res);
      *affected_out = (n && n[0]) ? atoi(n) : 0;
   }
   PQclear(res);
   return 0;
}

int aimee_pg_ping(void *pg_conn, char *errbuf, size_t errlen)
{
   return aimee_pg_exec(pg_conn, "SELECT 1", errbuf, errlen);
}

/* --- Prepared-statement surface ---------------------------------- */

struct aimee_pg_stmt
{
   PGconn *conn;
   char *sql;    /* rewritten :name → $N form */
   char **names; /* length = nparams */
   int nparams;
   char **values; /* text mode; NULL means SQL NULL */

   PGresult *result; /* set after first step */
   int nrows;
   int row_index; /* -1 before first step */
   int executed;
   unsigned char *blob_cache;
   size_t blob_cache_len;
   int blob_cache_col;
   int blob_cache_row;

   char errmsg[256];
};

static void pg_clear_blob_cache(aimee_pg_stmt_t *s)
{
   if (!s || !s->blob_cache)
      return;
   PQfreemem(s->blob_cache);
   s->blob_cache = NULL;
   s->blob_cache_len = 0;
   s->blob_cache_col = -1;
   s->blob_cache_row = -1;
}

aimee_pg_stmt_t *aimee_pg_prepare(void *pg_conn, const char *sql, char *errbuf, size_t errlen)
{
   if (!pg_conn || !sql)
      return NULL;

   char *rewritten = NULL;
   char **names = NULL;
   int nparams = 0;
   if (aimee_pg_rewrite_params(sql, &rewritten, &names, &nparams, errbuf, errlen) != 0)
      return NULL;

   aimee_pg_stmt_t *s = calloc(1, sizeof(*s));
   if (!s)
   {
      free(rewritten);
      aimee_pg_free_names(names, nparams);
      return NULL;
   }
   s->conn = (PGconn *)pg_conn;
   s->sql = rewritten;
   s->names = names;
   s->nparams = nparams;
   s->values = nparams > 0 ? calloc((size_t)nparams, sizeof(char *)) : NULL;
   s->row_index = -1;
   s->blob_cache_col = -1;
   s->blob_cache_row = -1;
   return s;
}

void aimee_pg_finalize(aimee_pg_stmt_t *s)
{
   if (!s)
      return;
   if (s->result)
      PQclear(s->result);
   pg_clear_blob_cache(s);
   if (s->values)
   {
      for (int i = 0; i < s->nparams; i++)
         free(s->values[i]);
      free(s->values);
   }
   aimee_pg_free_names(s->names, s->nparams);
   free(s->sql);
   free(s);
}

int aimee_pg_reset(aimee_pg_stmt_t *s)
{
   if (!s)
      return -1;
   pg_clear_blob_cache(s);
   if (s->result)
   {
      PQclear(s->result);
      s->result = NULL;
   }
   if (s->values)
   {
      for (int i = 0; i < s->nparams; i++)
      {
         free(s->values[i]);
         s->values[i] = NULL;
      }
   }
   s->nrows = 0;
   s->row_index = -1;
   s->executed = 0;
   return 0;
}

static int lookup_param(const aimee_pg_stmt_t *s, const char *name)
{
   if (!name || !*name)
      return -1;
   const char *key = (name[0] == ':' || name[0] == '@' || name[0] == '$') ? name + 1 : name;
   for (int i = 0; i < s->nparams; i++)
   {
      if (strcmp(s->names[i], key) == 0)
         return i;
   }
   return -1;
}

static int set_value(aimee_pg_stmt_t *s, int idx, const char *val)
{
   char *copy = val ? strdup(val) : NULL;
   if (val && !copy)
      return -1;
   free(s->values[idx]);
   s->values[idx] = copy;
   return 0;
}

int aimee_pg_bind_int(aimee_pg_stmt_t *s, const char *name, int v)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   char buf[32];
   snprintf(buf, sizeof(buf), "%d", v);
   return set_value(s, idx, buf);
}

int aimee_pg_bind_int64(aimee_pg_stmt_t *s, const char *name, int64_t v)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   char buf[32];
   snprintf(buf, sizeof(buf), "%lld", (long long)v);
   return set_value(s, idx, buf);
}

int aimee_pg_bind_double(aimee_pg_stmt_t *s, const char *name, double v)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   char buf[64];
   snprintf(buf, sizeof(buf), "%.17g", v);
   return set_value(s, idx, buf);
}

int aimee_pg_bind_text(aimee_pg_stmt_t *s, const char *name, const char *value)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   return set_value(s, idx, value);
}

int aimee_pg_bind_blob(aimee_pg_stmt_t *s, const char *name, const void *value, int len)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   size_t outlen = 0;
   unsigned char *escaped =
       PQescapeByteaConn(s->conn, (const unsigned char *)value, (size_t)len, &outlen);
   if (!escaped)
      return -1;
   char *copy = malloc(outlen);
   if (!copy)
   {
      PQfreemem(escaped);
      return -1;
   }
   memcpy(copy, escaped, outlen);
   PQfreemem(escaped);
   free(s->values[idx]);
   s->values[idx] = copy;
   return 0;
}

int aimee_pg_bind_null(aimee_pg_stmt_t *s, const char *name)
{
   int idx = lookup_param(s, name);
   if (idx < 0)
   {
      snprintf(s->errmsg, sizeof(s->errmsg), "unknown param :%s", name ? name : "");
      return -1;
   }
   free(s->values[idx]);
   s->values[idx] = NULL;
   return 0;
}

int aimee_pg_stmt_changes(aimee_pg_stmt_t *s)
{
   if (!s || !s->result)
      return 0;
   const char *n = PQcmdTuples(s->result);
   return (n && n[0]) ? atoi(n) : 0;
}

aimee_pg_step_t aimee_pg_step(aimee_pg_stmt_t *s, char *errbuf, size_t errlen)
{
   if (!s)
      return AIMEE_PG_ERR;

   if (!s->executed)
   {
      pg_clear_blob_cache(s);
      s->result = PQexecParams(s->conn, s->sql, s->nparams,
                               /*paramTypes=*/NULL, (const char *const *)s->values,
                               /*paramLengths=*/NULL,
                               /*paramFormats=*/NULL, /*resultFormat=*/0);
      ExecStatusType st = PQresultStatus(s->result);
      if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
      {
         copy_err(errbuf, errlen, PQresultErrorMessage(s->result));
         return AIMEE_PG_ERR;
      }
      s->executed = 1;
      s->nrows = (st == PGRES_TUPLES_OK) ? PQntuples(s->result) : 0;
      s->row_index = 0;
      return (s->nrows > 0) ? AIMEE_PG_ROW : AIMEE_PG_DONE;
   }

   pg_clear_blob_cache(s);
   s->row_index++;
   return (s->row_index < s->nrows) ? AIMEE_PG_ROW : AIMEE_PG_DONE;
}

int aimee_pg_column_count(aimee_pg_stmt_t *s)
{
   return s && s->result ? PQnfields(s->result) : 0;
}

const char *aimee_pg_column_name(aimee_pg_stmt_t *s, int col)
{
   if (!s || !s->result || col < 0 || col >= PQnfields(s->result))
      return NULL;
   return PQfname(s->result, col);
}

int aimee_pg_column_type(aimee_pg_stmt_t *s, int col)
{
   if (!s || !s->result || s->row_index < 0 || s->row_index >= s->nrows || col < 0 ||
       col >= PQnfields(s->result))
      return AIMEE_PG_VALUE_NULL;
   if (PQgetisnull(s->result, s->row_index, col))
      return AIMEE_PG_VALUE_NULL;

   Oid type = PQftype(s->result, col);
   switch (type)
   {
   case PG_TYPE_BYTEA:
      return AIMEE_PG_VALUE_BLOB;
   case PG_TYPE_BOOL:
   case PG_TYPE_INT2:
   case PG_TYPE_INT4:
   case PG_TYPE_INT8:
   case PG_TYPE_OID:
      return AIMEE_PG_VALUE_INTEGER;
   case PG_TYPE_FLOAT4:
   case PG_TYPE_FLOAT8:
   case PG_TYPE_NUMERIC:
      return AIMEE_PG_VALUE_FLOAT;
   case PG_TYPE_TEXT:
   case PG_TYPE_VARCHAR:
   case PG_TYPE_BPCHAR:
   case PG_TYPE_DATE:
   case PG_TYPE_TIME:
   case PG_TYPE_TIMETZ:
   case PG_TYPE_TIMESTAMP:
   case PG_TYPE_TIMESTAMPTZ:
   case PG_TYPE_JSON:
   case PG_TYPE_JSONB:
      return AIMEE_PG_VALUE_TEXT;
   default:
      return AIMEE_PG_VALUE_TEXT;
   }
}

const void *aimee_pg_column_blob(aimee_pg_stmt_t *s, int col)
{
   if (!s || !s->result || s->row_index < 0 || s->row_index >= s->nrows || col < 0 ||
       col >= PQnfields(s->result))
      return NULL;
   if (PQgetisnull(s->result, s->row_index, col))
      return NULL;
   if (s->blob_cache && s->blob_cache_row == s->row_index && s->blob_cache_col == col)
      return s->blob_cache;
   pg_clear_blob_cache(s);
   size_t outlen = 0;
   unsigned char *blob =
       PQunescapeBytea((const unsigned char *)PQgetvalue(s->result, s->row_index, col), &outlen);
   if (!blob)
      return NULL;
   s->blob_cache = blob;
   s->blob_cache_len = outlen;
   s->blob_cache_row = s->row_index;
   s->blob_cache_col = col;
   return s->blob_cache;
}

int aimee_pg_column_bytes(aimee_pg_stmt_t *s, int col)
{
   if (!aimee_pg_column_blob(s, col))
      return 0;
   return (int)s->blob_cache_len;
}

int aimee_pg_column_is_null(aimee_pg_stmt_t *s, int col)
{
   if (!s || !s->result || s->row_index < 0 || s->row_index >= s->nrows)
      return 1;
   return PQgetisnull(s->result, s->row_index, col) ? 1 : 0;
}

const char *aimee_pg_column_text(aimee_pg_stmt_t *s, int col)
{
   if (!s || !s->result || s->row_index < 0 || s->row_index >= s->nrows)
      return NULL;
   if (PQgetisnull(s->result, s->row_index, col))
      return NULL;
   return PQgetvalue(s->result, s->row_index, col);
}

int aimee_pg_column_int(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   return v ? atoi(v) : 0;
}

int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   return v ? (int64_t)strtoll(v, NULL, 10) : 0;
}

double aimee_pg_column_double(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   return v ? strtod(v, NULL) : 0.0;
}

#endif /* !AIMEE_DISABLE_POSTGRES */
