/* db2/css_insights.c: read-only CSS analysis signals. See css_insights.h. */
#include "css_insights.h"

#include "db2.h"
#include "db2_internal.h"
#include "db_postgres.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CSSI_ERRBUF 256

/* ── !important audit ──────────────────────────────────────────────────── */

int db2_css_important_audit(const char *pf, css_important_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (pf && pf[0]) ? 1 : 0;
   static const char *q_all = "SELECT p.name, d.property, COUNT(*) AS cnt, MIN(f.path) AS sample"
                              " FROM css_declarations d"
                              " JOIN css_rules c ON c.id = d.rule_id"
                              " JOIN files f ON f.id = c.file_id"
                              " JOIN projects p ON p.id = f.project_id"
                              " WHERE d.important = 1"
                              " GROUP BY p.name, d.property ORDER BY cnt DESC, d.property LIMIT ?1";
   static const char *q_filt =
       "SELECT p.name, d.property, COUNT(*) AS cnt, MIN(f.path) AS sample"
       " FROM css_declarations d"
       " JOIN css_rules c ON c.id = d.rule_id"
       " JOIN files f ON f.id = c.file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE d.important = 1 AND p.name = ?2"
       " GROUP BY p.name, d.property ORDER BY cnt DESC, d.property LIMIT ?1";
   char err[CSSI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? q_filt : q_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", pf);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_important_t *h = &out[n++];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *pr = aimee_pg_column_text(st, 1);
      const char *sm = aimee_pg_column_text(st, 3);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->property, sizeof(h->property), "%s", pr ? pr : "");
      h->count = aimee_pg_column_int(st, 2);
      snprintf(h->sample_file, sizeof(h->sample_file), "%s", sm ? sm : "");
   }
   aimee_pg_finalize(st);
   return n;
}

/* ── high-specificity (id-bearing) selectors ───────────────────────────── */

int db2_css_high_specificity(const char *pf, css_high_spec_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (pf && pf[0]) ? 1 : 0;
   static const char *q_all = "SELECT p.name, f.path, c.selector, c.spec_a, c.spec_b, c.spec_c,"
                              " c.line FROM css_rules c"
                              " JOIN files f ON f.id = c.file_id"
                              " JOIN projects p ON p.id = f.project_id"
                              " WHERE c.spec_a > 0"
                              " ORDER BY c.spec_a DESC, c.spec_b DESC, f.path LIMIT ?1";
   static const char *q_filt = "SELECT p.name, f.path, c.selector, c.spec_a, c.spec_b, c.spec_c,"
                               " c.line FROM css_rules c"
                               " JOIN files f ON f.id = c.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE c.spec_a > 0 AND p.name = ?2"
                               " ORDER BY c.spec_a DESC, c.spec_b DESC, f.path LIMIT ?1";
   char err[CSSI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? q_filt : q_all, err, sizeof(err));
   if (!st)
      return -1;
   aimee_pg_bind_int(st, "?1", max);
   if (filt)
      aimee_pg_bind_text(st, "?2", pf);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      css_high_spec_t *h = &out[n++];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 0);
      const char *fp = aimee_pg_column_text(st, 1);
      const char *sel = aimee_pg_column_text(st, 2);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      snprintf(h->selector, sizeof(h->selector), "%s", sel ? sel : "");
      h->spec_a = aimee_pg_column_int(st, 3);
      h->spec_b = aimee_pg_column_int(st, 4);
      h->spec_c = aimee_pg_column_int(st, 5);
      h->line = aimee_pg_column_int(st, 6);
   }
   aimee_pg_finalize(st);
   return n;
}

/* ── unused custom properties ──────────────────────────────────────────── */

/* A small set of referenced var() names (linear; distinct vars are bounded). */
#define CSSI_REFSET_MAX 8192
typedef struct
{
   char (*names)[CSS_PROPERTY_MAX];
   int n;
   int cap;
} refset_t;

static int refset_has(const refset_t *rs, const char *name)
{
   for (int i = 0; i < rs->n; i++)
      if (strcmp(rs->names[i], name) == 0)
         return 1;
   return 0;
}

static void refset_add(refset_t *rs, const char *name)
{
   if (!name[0] || rs->n >= CSSI_REFSET_MAX || refset_has(rs, name))
      return;
   if (rs->n >= rs->cap)
   {
      int nc = rs->cap == 0 ? 256 : rs->cap * 2;
      void *g = realloc(rs->names, (size_t)nc * CSS_PROPERTY_MAX);
      if (!g)
         return;
      rs->names = g;
      rs->cap = nc;
   }
   snprintf(rs->names[rs->n++], CSS_PROPERTY_MAX, "%s", name);
}

/* Collect every `--name` referenced via var() in a value. */
static void scan_var_refs(const char *val, refset_t *rs)
{
   const char *s = val;
   while ((s = strstr(s, "var(")) != NULL)
   {
      s += 4;
      while (*s && isspace((unsigned char)*s))
         s++;
      if (s[0] == '-' && s[1] == '-')
      {
         char name[CSS_PROPERTY_MAX];
         size_t j = 0;
         while (*s && (isalnum((unsigned char)*s) || *s == '-' || *s == '_') &&
                j < sizeof(name) - 1)
            name[j++] = *s++;
         name[j] = '\0';
         refset_add(rs, name);
      }
   }
}

int db2_css_unused_custom_properties(const char *pf, css_unused_var_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (pf && pf[0]) ? 1 : 0;
   char err[CSSI_ERRBUF] = "";
   refset_t rs = {0};

   /* Pass 1: collect referenced var() names from values that mention var(. */
   static const char *r_all = "SELECT d.value FROM css_declarations d"
                              " WHERE d.value LIKE '%var(%'";
   static const char *r_filt = "SELECT d.value FROM css_declarations d"
                               " JOIN css_rules c ON c.id = d.rule_id"
                               " JOIN files f ON f.id = c.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE d.value LIKE '%var(%' AND p.name = ?1";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? r_filt : r_all, err, sizeof(err));
   if (!st)
   {
      free(rs.names);
      return -1;
   }
   if (filt)
      aimee_pg_bind_text(st, "?1", pf);
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         scan_var_refs(v, &rs);
   }
   aimee_pg_finalize(st);

   /* Pass 2: each declared --var (deduped) not in the referenced set is unused. */
   static const char *d_all =
       "SELECT d.property, MIN(p.name), MIN(f.path), MIN(c.line)"
       " FROM css_declarations d"
       " JOIN css_rules c ON c.id = d.rule_id"
       " JOIN files f ON f.id = c.file_id"
       " JOIN projects p ON p.id = f.project_id"
       " WHERE d.property LIKE '--%' GROUP BY d.property ORDER BY d.property";
   static const char *d_filt = "SELECT d.property, MIN(p.name), MIN(f.path), MIN(c.line)"
                               " FROM css_declarations d"
                               " JOIN css_rules c ON c.id = d.rule_id"
                               " JOIN files f ON f.id = c.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE d.property LIKE '--%' AND p.name = ?1"
                               " GROUP BY d.property ORDER BY d.property";
   st = aimee_pg_prepare(conn, filt ? d_filt : d_all, err, sizeof(err));
   if (!st)
   {
      free(rs.names);
      return -1;
   }
   if (filt)
      aimee_pg_bind_text(st, "?1", pf);
   int n = 0;
   while (n < max && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *name = aimee_pg_column_text(st, 0);
      if (!name || !name[0] || refset_has(&rs, name))
         continue;
      css_unused_var_t *h = &out[n++];
      memset(h, 0, sizeof(*h));
      const char *pn = aimee_pg_column_text(st, 1);
      const char *fp = aimee_pg_column_text(st, 2);
      snprintf(h->name, sizeof(h->name), "%s", name);
      snprintf(h->project, sizeof(h->project), "%s", pn ? pn : "");
      snprintf(h->file_path, sizeof(h->file_path), "%s", fp ? fp : "");
      h->line = aimee_pg_column_int(st, 3);
   }
   aimee_pg_finalize(st);
   free(rs.names);
   return n;
}

/* ── design-token candidates ───────────────────────────────────────────── */

#define CSSI_TOKMAP_MAX 16384
typedef struct
{
   css_token_cand_t *items;
   int n;
   int cap;
} tokmap_t;

static void tokmap_bump(tokmap_t *m, const char *value, const char *kind)
{
   for (int i = 0; i < m->n; i++)
      if (strcmp(m->items[i].value, value) == 0)
      {
         m->items[i].count++;
         return;
      }
   if (m->n >= CSSI_TOKMAP_MAX)
      return;
   if (m->n >= m->cap)
   {
      int nc = m->cap == 0 ? 256 : m->cap * 2;
      void *g = realloc(m->items, (size_t)nc * sizeof(css_token_cand_t));
      if (!g)
         return;
      m->items = g;
      m->cap = nc;
   }
   css_token_cand_t *it = &m->items[m->n++];
   memset(it, 0, sizeof(*it));
   snprintf(it->value, sizeof(it->value), "%s", value);
   snprintf(it->kind, sizeof(it->kind), "%s", kind);
   it->count = 1;
}

static int hexlen_ok(int n)
{
   return n == 3 || n == 4 || n == 6 || n == 8;
}

/* Extract colour (#hex, rgb/rgba/hsl/hsla()) and length (px/rem/em) literals
 * from one declaration value into the count map. Values built only from var()
 * yield nothing (already tokenised). */
static void scan_literals(const char *val, tokmap_t *m)
{
   const char *s = val;
   while (*s)
   {
      if (*s == '#')
      {
         const char *h = s + 1;
         int cnt = 0;
         while (isxdigit((unsigned char)*h))
         {
            h++;
            cnt++;
         }
         if (hexlen_ok(cnt))
         {
            char lit[CSS_VALUE_MAX];
            size_t L = (size_t)(h - s);
            if (L < sizeof(lit))
            {
               for (size_t i = 0; i < L; i++)
                  lit[i] = (char)tolower((unsigned char)s[i]);
               lit[L] = '\0';
               tokmap_bump(m, lit, "color");
            }
            s = h;
            continue;
         }
         s = h;
         continue;
      }
      if (strncasecmp(s, "rgb(", 4) == 0 || strncasecmp(s, "rgba(", 5) == 0 ||
          strncasecmp(s, "hsl(", 4) == 0 || strncasecmp(s, "hsla(", 5) == 0)
      {
         const char *p = strchr(s, ')');
         if (p)
         {
            char lit[CSS_VALUE_MAX];
            size_t L = (size_t)(p + 1 - s);
            if (L < sizeof(lit))
            {
               /* normalise interior whitespace out for stable grouping */
               size_t j = 0;
               for (const char *q = s; q <= p && j < sizeof(lit) - 1; q++)
                  if (!isspace((unsigned char)*q))
                     lit[j++] = (char)tolower((unsigned char)*q);
               lit[j] = '\0';
               tokmap_bump(m, lit, "color");
            }
            s = p + 1;
            continue;
         }
      }
      /* length: a number at a word boundary, immediately followed by px/rem/em. */
      if (isdigit((unsigned char)*s) &&
          (s == val || (!isalnum((unsigned char)s[-1]) && s[-1] != '.')))
      {
         const char *num = s;
         while (isdigit((unsigned char)*s) || *s == '.')
            s++;
         int ulen = 0;
         if (strncasecmp(s, "rem", 3) == 0)
            ulen = 3;
         else if (strncasecmp(s, "px", 2) == 0 || strncasecmp(s, "em", 2) == 0)
            ulen = 2;
         if (ulen && !isalpha((unsigned char)s[ulen]))
         {
            size_t L = (size_t)(s + ulen - num);
            char lit[CSS_VALUE_MAX];
            int is_zero = 1;
            for (const char *q = num; q < s; q++)
               if (*q != '0' && *q != '.')
                  is_zero = 0;
            if (!is_zero && L < sizeof(lit))
            {
               for (size_t i = 0; i < L; i++)
                  lit[i] = (char)tolower((unsigned char)num[i]);
               lit[L] = '\0';
               tokmap_bump(m, lit, "length");
            }
            s += ulen;
         }
         continue;
      }
      s++;
   }
}

static int tok_cmp_desc(const void *a, const void *b)
{
   const css_token_cand_t *x = a, *y = b;
   if (x->count != y->count)
      return y->count - x->count;
   return strcmp(x->value, y->value);
}

int db2_css_token_candidates(const char *pf, int min_count, css_token_cand_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (min_count < 2)
      min_count = 2;
   void *conn = db2_conn();
   if (!conn)
      return -1;
   int filt = (pf && pf[0]) ? 1 : 0;
   static const char *q_all = "SELECT d.value FROM css_declarations d WHERE d.value <> ''";
   static const char *q_filt = "SELECT d.value FROM css_declarations d"
                               " JOIN css_rules c ON c.id = d.rule_id"
                               " JOIN files f ON f.id = c.file_id"
                               " JOIN projects p ON p.id = f.project_id"
                               " WHERE d.value <> '' AND p.name = ?1";
   char err[CSSI_ERRBUF] = "";
   aimee_pg_stmt_t *st = aimee_pg_prepare(conn, filt ? q_filt : q_all, err, sizeof(err));
   if (!st)
      return -1;
   if (filt)
      aimee_pg_bind_text(st, "?1", pf);
   tokmap_t m = {0};
   while (aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *v = aimee_pg_column_text(st, 0);
      if (v)
         scan_literals(v, &m);
   }
   aimee_pg_finalize(st);

   if (m.n > 1)
      qsort(m.items, (size_t)m.n, sizeof(css_token_cand_t), tok_cmp_desc);
   int n = 0;
   for (int i = 0; i < m.n && n < max; i++)
      if (m.items[i].count >= min_count)
         out[n++] = m.items[i];
   free(m.items);
   return n;
}
