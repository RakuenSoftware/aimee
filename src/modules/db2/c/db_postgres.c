/* libpq-backed implementation for DB2, the shared knowledge tier. See
 * docs/STORAGE_TIERS.md. */

#include "db_postgres.h"

#include "db2_pool.h" /* DB2_POOL_HOLD_CEILING_MS — the pool owns the figure */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp: boolean text spellings from libpq */

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
      {
         errno = ENOMEM;
         return -1;
      }
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
      {
         errno = ENOMEM;
         return -1;
      }
      *names = tmp;
      *nn_cap = ncap;
   }
   (*names)[*nn] = strdup(name);
   if (!(*names)[*nn])
   {
      errno = ENOMEM;
      return -1;
   }
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
 * DB2 text-search block in src/modules/db2/c/schema.sql. */
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
   {
      errno = ENOMEM;
      return NULL;
   }
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
            errno = ENOMEM;                                                                        \
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
   {
      errno = ENOMEM;
      return NULL;
   }
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
            errno = ENOMEM;                                                                        \
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
   {
      errno = EINVAL;
      return -1;
   }

   /* Step 1a: number bare positional `?` placeholders so the FTS
    * rewriter can duplicate them (MATCH arg reused by bm25) and still
    * dedupe to one $N downstream. */
   errno = 0;
   char *xlated = prenumber_positional_placeholders(sql_in);
   if (!xlated && errno == ENOMEM)
      goto resource_failure;
   if (xlated)
      sql_in = xlated;

   /* Step 1b: translate text-search operators (MATCH, bm25, INSERT INTO
    * *_fts) into DB2 tsvector equivalents. */
   errno = 0;
   char *fts_xlated = rewrite_db2_text_search(sql_in);
   if (!fts_xlated && errno == ENOMEM)
      goto resource_failure;
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
      goto resource_failure;
   }
   out[0] = '\0';
   size_t olen = 0;

   char **names = NULL;
   int nn = 0, nn_cap = 0;

#define REWRITE_APPEND(src, n)                                                                     \
   do                                                                                              \
   {                                                                                               \
      if (buf_append(&out, &cap, &olen, (src), (n)) != 0)                                          \
         goto resource_failure_with_output;                                                        \
   } while (0)

   const char *p = sql_in;
   while (*p)
   {
      /* Single-quoted string literal: copy verbatim, honoring the
       * SQL '' escape for embedded quotes. */
      if (*p == '\'')
      {
         REWRITE_APPEND(p, 1);
         p++;
         while (*p)
         {
            if (p[0] == '\'' && p[1] == '\'')
            {
               REWRITE_APPEND(p, 2);
               p += 2;
               continue;
            }
            REWRITE_APPEND(p, 1);
            if (*p++ == '\'')
               break;
         }
         continue;
      }

      /* Double-quoted identifier: copy verbatim. */
      if (*p == '"')
      {
         REWRITE_APPEND(p, 1);
         p++;
         while (*p && *p != '"')
         {
            REWRITE_APPEND(p, 1);
            p++;
         }
         if (*p)
         {
            REWRITE_APPEND(p, 1);
            p++;
         }
         continue;
      }

      /* Line comment -- … \n */
      if (p[0] == '-' && p[1] == '-')
      {
         while (*p && *p != '\n')
         {
            REWRITE_APPEND(p, 1);
            p++;
         }
         continue;
      }

      /* SQL block comment: open and close sequences use slash-star / star-slash. */
      if (p[0] == '/' && p[1] == '*')
      {
         REWRITE_APPEND(p, 2);
         p += 2;
         while (*p && !(p[0] == '*' && p[1] == '/'))
         {
            REWRITE_APPEND(p, 1);
            p++;
         }
         if (*p)
         {
            REWRITE_APPEND(p, 2);
            p += 2;
         }
         continue;
      }

      /* Postgres :: cast operator — pass through. */
      if (p[0] == ':' && p[1] == ':')
      {
         REWRITE_APPEND(p, 2);
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
            errno = EINVAL;
            return -1;
         }
         char name[128];
         memcpy(name, start, len);
         name[len] = '\0';

         int idx = append_param_name(&names, &nn, &nn_cap, name);
         if (idx < 0)
            goto resource_failure_with_output;

         char buf[24];
         int nprinted = snprintf(buf, sizeof(buf), "$%d", idx + 1);
         REWRITE_APPEND(buf, (size_t)nprinted);
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
            goto resource_failure_with_output;
         char buf[24];
         int nprinted = snprintf(buf, sizeof(buf), "$%d", idx + 1);
         REWRITE_APPEND(buf, (size_t)nprinted);
         continue;
      }

      REWRITE_APPEND(p, 1);
      p++;
   }

   *out_sql = out;
   *out_names = names;
   *out_count = nn;
   free(xlated);
#undef REWRITE_APPEND
   return 0;

resource_failure_with_output:
   free(out);
   aimee_pg_free_names(names, nn);
resource_failure:
   free(xlated);
   errno = ENOMEM;
   if (errbuf && errlen)
      snprintf(errbuf, errlen, "out of memory rewriting SQL");
#undef REWRITE_APPEND
   return -1;
}

/* --- libpq-dependent code below ---------------------------------- */

/* Not a second opinion on the figure — the pool owns it. */
#define DB2_DEFAULT_STATEMENT_TIMEOUT_MS DB2_POOL_HOLD_CEILING_MS

/* Same figure as the statement bound, and for the same reason: a lease must not
 * outlive the pool's hold ceiling. statement_timeout only bounds a statement, so
 * a unit of work that OPENS a transaction and then stalls before its next
 * statement — waiting on a lock, an external call, anything not SQL — is
 * invisible to it and holds its pool member indefinitely. Measured: leases held
 * ~4.5 hours against a 5-minute ceiling, with statement_timeout correctly set
 * the whole time, because nothing was executing to time out.
 *
 * Postgres ends the backend itself, so the blocked thread's next call fails, it
 * unwinds, and the lease is RETURNED — the pool recovers without a restart,
 * which is the outcome the reaper's give-up path cannot produce. */
#define DB2_DEFAULT_IDLE_IN_TRANSACTION_TIMEOUT_MS DB2_POOL_HOLD_CEILING_MS
#define DB2_CONNECT_TIMEOUT_S                      "10"

/* Out-of-range is a typo like any other, and the truncating cast makes it the
 * WORST kind: "4294967296" narrows to int 0, which is the sentinel for "no
 * statement timeout". An unchecked overflow therefore does not merely pick a
 * strange bound, it silently removes the bound entirely — the exact outcome this
 * function exists to make impossible. errno must be inspected because strtol
 * saturates at LONG_MAX/LONG_MIN rather than reporting failure in its return. */
/* Takes the RAW value, not the variable name: the reference-doc generator scans
 * for literal getenv("AIMEE_*") calls, so hiding the lookup behind a parameter
 * makes the variable invisible to it — which silently dropped a documented
 * variable from docs/gen the first time this was extracted. Each wrapper keeps
 * its own getenv literal; the validation stays shared. */
static int db2_pg_timeout_ms_parse(const char *raw, int fallback)
{
   if (!raw || !raw[0])
      return fallback;

   /* Canonical decimal only, checked before strtol rather than after: digits, and
    * no redundant leading zero.
    *
    * strtol accepts " 0", "\t0", "+0" and "-0" and returns 0 — and 0 is the
    * sentinel that DISABLES the bound — so each of those malformed spellings
    * silently opted out of the safety property. Digits-only fixed those but still
    * let "00" through, and "00" is not the documented opt-out; the contract is
    * that EXACTLY "0" disables and everything else malformed falls back. The
    * leading-zero rule makes that literally true, and it also stops "007" quietly
    * meaning a 7ms timeout.
    *
    * This is the fourth way this function has been able to reach "unbounded" by
    * accident, after the unchecked overflow, the unchecked errno and the signed
    * zeroes. Hence a rule that rejects by construction rather than a fourth
    * enumeration of bad spellings. */
   for (const char *c = raw; *c; c++)
      if (*c < '0' || *c > '9')
         return fallback;
   if (raw[0] == '0' && raw[1])
      return fallback;

   char *end = NULL;
   errno = 0;
   long v = strtol(raw, &end, 10);
   if (!end || *end || errno == ERANGE || v < 0 || v > INT_MAX)
      return fallback; /* a typo must not remove the bound */
   return (int)v;
}

int db2_pg_statement_timeout_ms(void)
{
   return db2_pg_timeout_ms_parse(getenv("AIMEE_DB2_STATEMENT_TIMEOUT_MS"),
                                  DB2_DEFAULT_STATEMENT_TIMEOUT_MS);
}

int db2_pg_idle_in_transaction_timeout_ms(void)
{
   return db2_pg_timeout_ms_parse(getenv("AIMEE_DB2_IDLE_IN_TRANSACTION_TIMEOUT_MS"),
                                  DB2_DEFAULT_IDLE_IN_TRANSACTION_TIMEOUT_MS);
}

/* PQconnectdb accepts EITHER a keyword/value string ("host=db user=aimee") OR a
 * URI ("postgresql://user@host/db"), and the two forms cannot be mixed. Appending
 * " connect_timeout=10" to a URI is not merely ineffective: libpq parses it
 * without complaint and folds the trailing text into the DATABASE NAME, yielding
 * dbname="aimee_shared connect_timeout=10 ...". The connection then fails on a
 * database that does not exist, and the bounds are silently absent — so the
 * mistake costs both the connection and the property it was meant to add.
 *
 * Deployments set AIMEE_DB2_URL as a URI, so both forms must be handled: a URI
 * takes its parameters in the query string, joined with '&' after a '?'.
 *
 * Both URI schemes count. libpq accepts postgres:// as well as postgresql://, so
 * recognising only the longer one would put a postgres:// deployment straight
 * back into the mixed-form bug above. */
static int conninfo_is_uri(const char *s)
{
   return strncmp(s, "postgresql://", 13) == 0 || strncmp(s, "postgres://", 11) == 0;
}

/* Does the conninfo already set this exact option?
 *
 * This has to walk the string's real grammar, not search it. A bare strstr() was
 * wrong in three ways, each a live defect: it matched INSIDE values, so
 * dbname=keepalives_test suppressed keepalives on an ordinary conninfo; it
 * matched keys sharing a prefix, so keepalives_idle=30 suppressed the whole
 * group; and checking only for a preceding separator still matches option-shaped
 * text inside a QUOTED value, so dbname='tenant keepalives=off' — a legal
 * conninfo, and a tenant name is attacker-influenced in a multi-tenant
 * deployment — silently dropped the bounds.
 *
 * Every one of those failures is silent and in the unsafe direction: the option
 * appears set, so the bound is not added. So the scan tracks where values begin
 * and end, honouring single quotes and backslash escapes for the keyword/value
 * form, and looks only at the query component of a URI. */
static int hexval(int c)
{
   if (c >= '0' && c <= '9')
      return c - '0';
   if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
   if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
   return -1;
}

/* Compare one URI query key against `key`, decoding percent-escapes as libpq
 * does. A raw byte compare misses connect%5Ftimeout=3 — which libpq reads as
 * connect_timeout=3 — so the bound gets appended a second time and, since the
 * later value wins, silently overrides what the caller asked for. */
static int uri_key_equals(const char *p, const char *key)
{
   size_t i = 0;
   while (*p && *p != '=' && *p != '&')
   {
      int c = (unsigned char)*p;
      if (c == '%' && hexval((unsigned char)p[1]) >= 0 && hexval((unsigned char)p[2]) >= 0)
      {
         c = hexval((unsigned char)p[1]) * 16 + hexval((unsigned char)p[2]);
         p += 3;
      }
      else
         p++;
      if (key[i] == '\0' || (unsigned char)key[i] != (unsigned char)c)
         return 0;
      i++;
   }
   return key[i] == '\0' && *p == '=';
}

static int conninfo_has_key(const char *base, const char *key)
{
   size_t klen = strlen(key);

   if (conninfo_is_uri(base))
   {
      /* Only the query string holds options. Anything earlier is userinfo, host
       * or path — a password or database name there must never be read as one. */
      const char *p = strchr(base, '?');
      if (!p)
         return 0;
      for (p++; *p;)
      {
         if (uri_key_equals(p, key))
            return 1;
         const char *amp = strchr(p, '&');
         if (!amp)
            break;
         p = amp + 1;
      }
      return 0;
   }

   for (const char *p = base; *p;)
   {
      while (isspace((unsigned char)*p))
         p++;
      if (!*p)
         break;
      const char *kstart = p;
      while (*p && *p != '=' && !isspace((unsigned char)*p))
         p++;
      size_t this_len = (size_t)(p - kstart);
      while (isspace((unsigned char)*p))
         p++;
      if (*p != '=')
      {
         /* Not a key=value pair; the token is junk. Nothing was consumed by the
          * value scan below, so step one byte to guarantee progress. */
         if (p == kstart)
            p++;
         continue;
      }
      p++;
      while (isspace((unsigned char)*p))
         p++;
      int match = (this_len == klen && strncmp(kstart, key, klen) == 0);
      if (*p == '\'')
      {
         for (p++; *p && *p != '\''; p++)
            if (*p == '\\' && p[1])
               p++;
         if (*p == '\'')
            p++;
      }
      else
      {
         for (; *p && !isspace((unsigned char)*p); p++)
            if (*p == '\\' && p[1])
               p++;
      }
      if (match)
         return 1;
   }
   return 0;
}

/* Append the safety parameters unless the caller already set them, so an
 * explicit conninfo still wins. */
int db2_pg_conninfo_with_bounds(const char *conninfo, char *out, size_t out_sz)
{
   const char *base = conninfo ? conninfo : "";
   int uri = conninfo_is_uri(base);
   /* First added parameter opens the query string; the rest extend it.
    *
    * A URI already ending in '?' or '&' needs NO separator. Adding one there
    * creates an empty query parameter, and libpq rejects that outright —
    * "missing key/value separator" — so the whole connection fails rather than
    * merely losing a bound. `postgresql://h/db?` is a conninfo libpq accepts on
    * its own, so this is reachable from a legal input. */
   size_t blen = strlen(base);
   char last = blen ? base[blen - 1] : '\0';
   const char *sep1;
   if (!uri)
      sep1 = " ";
   else if (last == '?' || last == '&')
      sep1 = "";
   else
      sep1 = strchr(base, '?') ? "&" : "?";
   const char *sep = uri ? "&" : " ";

   char params[256];
   int n = 0;
   params[0] = '\0';

/* Add a key only if the caller did not set that exact key, so their value always
 * wins and nothing is ever specified twice. */
#define ADD_IF_UNSET(key, val)                                                                     \
   do                                                                                              \
   {                                                                                               \
      if (!conninfo_has_key(base, (key)) && n >= 0 && (size_t)n < sizeof params)                   \
         n += snprintf(params + n, sizeof params - (size_t)n, "%s%s=%s", n ? sep : sep1, (key),    \
                       (val));                                                                     \
   } while (0)

   ADD_IF_UNSET("connect_timeout", DB2_CONNECT_TIMEOUT_S);

   /* One uniform rule for every key: the caller's value wins where they set one,
    * ours fills in where they did not. No key is special-cased.
    *
    * An earlier version treated an explicit `keepalives=` as the caller owning
    * the whole group and added none of the tuning keys. That was wrong for
    * `keepalives=1`: the caller is ASKING for keepalives, and withholding the
    * tuning leaves libpq's default idle — 7200s — where this code intends 30s,
    * so the setting meant to detect a vanished peer would take two hours to do
    * it. It also made the invariant conditional, which is how the gap hid.
    *
    * With `keepalives=0` the tuning keys are still emitted and simply unused;
    * carrying three ignored settings is a cosmetic cost, and it buys a rule with
    * no exception to forget. */
   ADD_IF_UNSET("keepalives", "1");
   ADD_IF_UNSET("keepalives_idle", "30");
   ADD_IF_UNSET("keepalives_interval", "10");
   ADD_IF_UNSET("keepalives_count", "3");
#undef ADD_IF_UNSET

   /* Two ways to get this wrong, and the first version chose the second one.
    * Truncating yields a malformed conninfo. Returning the base unchanged yields
    * a well-formed but UNBOUNDED one — which quietly breaks the guarantee that
    * every connection carries these bounds, on the long TLS conninfo most likely
    * to need them. Report the failure instead and let the caller refuse. */
   if (strlen(base) + strlen(params) + 1 > out_sz)
   {
      if (out_sz)
         out[0] = '\0';
      return -1;
   }
   snprintf(out, out_sz, "%s%s", base, params);
   return 0;
}

/* Statement counter for the test-only $(OBJDIR)/db2/ object namespace, so the
 * SAME test can assert round-trip count against real libpq as against the
 * sqlite shim. Never defined for a production build: tests/Rules.mk sets
 * AIMEE_PG_STMT_COUNTER on that object alone, the way it already sets
 * AIMEE_TEST_PG_BACKEND on db2_test_shim.o. */
#ifdef AIMEE_PG_STMT_COUNTER
static long s_pg_stmt_count = 0;
void aimee_pg_test_stmt_count_reset(void)
{
   s_pg_stmt_count = 0;
}
long aimee_pg_test_stmt_count(void)
{
   return s_pg_stmt_count;
}
#define AIMEE_PG_STMT_TICK() (s_pg_stmt_count++)
#else
#define AIMEE_PG_STMT_TICK() ((void)0)
#endif

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
int aimee_pg_exec_sqlstate(void *c, const char *s, char state[6], char *e, size_t n)
{
   if (state)
      state[0] = '\0';
   return aimee_pg_exec(c, s, e, n);
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
   return aimee_pg_prepare_ex(c, s, NULL, e, n);
}
aimee_pg_stmt_t *aimee_pg_prepare_ex(void *c, const char *s, aimee_pg_prepare_error_t *kind,
                                     char *e, size_t n)
{
   AIMEE_PG_STMT_TICK();
   (void)c;
   (void)s;
   if (kind)
      *kind = AIMEE_PG_PREPARE_RESOURCE;
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
const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   (void)s;
   return "";
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

/* Bounds every connection this process opens, so no single operation can hold a
 * pool lease indefinitely.
 *
 * This exists because there were no bounds here at all — not because any outage
 * was traced to their absence. The reaper deliberately will not reclaim a live
 * thread's connection (libpq forbids concurrent use), so an over-ceiling lease is
 * something the pool can report but not stop. With no statement_timeout, a query
 * that never returns is indistinguishable from one that is merely slow, and
 * bounding the statement is the only lever available at this layer.
 *
 * The pool already declares 300s as the longest a lease may reasonably be held.
 * That figure is now ENFORCED at the server rather than only observed after the
 * fact: a statement cannot outlive the ceiling that defines "stuck".
 *
 * Keepalives matter for the same reason — a peer that vanishes without a FIN
 * leaves a read blocked forever, which no statement_timeout can interrupt
 * because the server never sees the query.
 *
 * The default is the pool's own DB2_POOL_HOLD_CEILING_MS, so the two cannot
 * drift apart.
 *
 * AIMEE_DB2_STATEMENT_TIMEOUT_MS overrides the default for a deployment with
 * genuinely longer work; 0 disables it, which restores the old unbounded
 * behaviour and should be a deliberate choice. */
void *aimee_pg_open(const char *conninfo, char *errbuf, size_t errlen)
{
   char bounded[2048];
   if (db2_pg_conninfo_with_bounds(conninfo, bounded, sizeof bounded) != 0)
   {
      copy_err(errbuf, errlen, "conninfo too long to carry the connection safety bounds");
      return NULL;
   }

   PGconn *conn = PQconnectdb(bounded);
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

   /* Both bounds, applied the same way and refused the same way. The idle bound
    * is what releases a lease whose holder is stalled OUTSIDE a statement — the
    * case statement_timeout structurally cannot see. */
   int timeout_ms = db2_pg_statement_timeout_ms();
   int idle_ms = db2_pg_idle_in_transaction_timeout_ms();
   if (timeout_ms > 0 || idle_ms > 0)
   {
      char sql[192];
      if (timeout_ms > 0 && idle_ms > 0)
         snprintf(sql, sizeof sql,
                  "SET statement_timeout = %d; SET idle_in_transaction_session_timeout = %d",
                  timeout_ms, idle_ms);
      else if (timeout_ms > 0)
         snprintf(sql, sizeof sql, "SET statement_timeout = %d", timeout_ms);
      else
         snprintf(sql, sizeof sql, "SET idle_in_transaction_session_timeout = %d", idle_ms);
      PGresult *r = PQexec(conn, sql);
      int ok = r && PQresultStatus(r) == PGRES_COMMAND_OK;
      if (r)
         PQclear(r);
      if (!ok)
      {
         /* Refuse the connection rather than hand back an unbounded one. A
          * caller that asked for bounds and silently got none is worse than a
          * caller that fails: the pool would carry a member it cannot reclaim,
          * with nothing in the connection to say so. */
         /* Keep libpq's reason: a refused connection is otherwise
          * indistinguishable between a permission error, an ALTER ROLE
          * restriction and a connection-state problem. */
         char why[512];
         snprintf(why, sizeof why, "could not set statement_timeout on the connection: %s",
                  PQerrorMessage(conn));
         copy_err(errbuf, errlen, why);
         PQfinish(conn);
         return NULL;
      }
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

int aimee_pg_exec_sqlstate(void *pg_conn, const char *sql, char state[6], char *errbuf,
                           size_t errlen)
{
   if (state)
      state[0] = '\0';
   if (!pg_conn || !sql)
      return -1;
   PGresult *res = PQexec((PGconn *)pg_conn, sql);
   ExecStatusType st = PQresultStatus(res);
   if (st != PGRES_COMMAND_OK && st != PGRES_TUPLES_OK)
   {
      const char *value = PQresultErrorField(res, PG_DIAG_SQLSTATE);
      if (state)
         snprintf(state, 6, "%s", value ? value : "");
      copy_err(errbuf, errlen, PQresultErrorMessage(res));
      PQclear(res);
      return -1;
   }
   PQclear(res);
   return 0;
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
   char sqlstate[6];
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
   return aimee_pg_prepare_ex(pg_conn, sql, NULL, errbuf, errlen);
}

aimee_pg_stmt_t *aimee_pg_prepare_ex(void *pg_conn, const char *sql, aimee_pg_prepare_error_t *kind,
                                     char *errbuf, size_t errlen)
{
   AIMEE_PG_STMT_TICK();
   if (kind)
      *kind = AIMEE_PG_PREPARE_INVALID;
   if (!pg_conn || !sql)
      return NULL;

   char *rewritten = NULL;
   char **names = NULL;
   int nparams = 0;
   if (aimee_pg_rewrite_params(sql, &rewritten, &names, &nparams, errbuf, errlen) != 0)
   {
      if (kind && errno == ENOMEM)
         *kind = AIMEE_PG_PREPARE_RESOURCE;
      return NULL;
   }

   aimee_pg_stmt_t *s = calloc(1, sizeof(*s));
   if (!s)
   {
      if (kind)
         *kind = AIMEE_PG_PREPARE_RESOURCE;
      free(rewritten);
      aimee_pg_free_names(names, nparams);
      return NULL;
   }
   s->conn = (PGconn *)pg_conn;
   s->sql = rewritten;
   s->names = names;
   s->nparams = nparams;
   s->values = nparams > 0 ? calloc((size_t)nparams, sizeof(char *)) : NULL;
   if (nparams > 0 && !s->values)
   {
      if (kind)
         *kind = AIMEE_PG_PREPARE_RESOURCE;
      aimee_pg_free_names(s->names, s->nparams);
      free(s->sql);
      free(s);
      return NULL;
   }
   s->row_index = -1;
   s->blob_cache_col = -1;
   s->blob_cache_row = -1;
   if (kind)
      *kind = AIMEE_PG_PREPARE_OK;
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
   s->sqlstate[0] = '\0';
   return 0;
}

const char *aimee_pg_sqlstate(const aimee_pg_stmt_t *s)
{
   return s ? s->sqlstate : "";
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
         const char *state = PQresultErrorField(s->result, PG_DIAG_SQLSTATE);
         snprintf(s->sqlstate, sizeof(s->sqlstate), "%s", state ? state : "");
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

/* Postgres renders BOOLEAN as "t"/"f" in the text format libpq hands back, so the
 * numeric accessors below would read every true as 0 -- strtoll stops at the 't'.
 * The sqlite shim stores booleans as 0/1 integers and has always returned them
 * correctly, so a boolean column read through aimee_pg_column_int silently flipped
 * to false the moment the same code ran against the real engine (audit_events
 * .flagged_for_review and mining_jobs.enabled are both read this way). Recognise the
 * two spellings here, at the one place the impedance mismatch actually lives, rather
 * than casting at each of the call sites. No integer or float literal Postgres emits
 * begins with 't' or 'f', so this cannot shadow a real numeric value.
 *
 * Returns 1 for true, 0 for false, -1 when `v` is not a boolean spelling. */
static int pg_boolean_value(const char *v)
{
   if (!v)
      return -1;
   if ((v[0] == 't' || v[0] == 'T') && (v[1] == '\0' || strcasecmp(v, "true") == 0))
      return 1;
   if ((v[0] == 'f' || v[0] == 'F') && (v[1] == '\0' || strcasecmp(v, "false") == 0))
      return 0;
   return -1;
}

int aimee_pg_column_int(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   if (!v)
      return 0;
   int b = pg_boolean_value(v);
   return b >= 0 ? b : atoi(v);
}

int64_t aimee_pg_column_int64(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   if (!v)
      return 0;
   int b = pg_boolean_value(v);
   return b >= 0 ? (int64_t)b : (int64_t)strtoll(v, NULL, 10);
}

double aimee_pg_column_double(aimee_pg_stmt_t *s, int col)
{
   const char *v = aimee_pg_column_text(s, col);
   if (!v)
      return 0.0;
   int b = pg_boolean_value(v);
   return b >= 0 ? (double)b : strtod(v, NULL);
}

#endif /* !AIMEE_DISABLE_POSTGRES */
