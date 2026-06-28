/* cross_repo_identity.c: the repo-identity layer (H0c, precision-hardening §1.5).
 * Pure manifest -> identity extraction + a transactional rebuild of the
 * cross_repo_identity table from indexed manifest file_contents. See
 * cross_repo_identity.h and docs/proposals/pending/cross-repo-precision-hardening.md. */

#include "cross_repo_identity.h"

#include "cross_repo_deps.h" /* xrepo_parse_module_id */

#include "aimee.h"
#include "db2.h"
#include "db_postgres.h"
#include "log.h"

#include <stdio.h>
#include <string.h>

#define CRI_LOG_TAG "cross_repo.identity"
#define CRI_ERRBUF  256

/* ---- pure parsers -------------------------------------------------------- */

static const char *path_basename(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int ends_with(const char *s, const char *suf)
{
   size_t sl = strlen(s), fl = strlen(suf);
   return sl >= fl && strcmp(s + sl - fl, suf) == 0;
}

static int cmake_ident_char(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

/* Read the first CMake argument identifier after `(` into out: skip whitespace
 * (incl. newlines), accept [A-Za-z0-9_.+:-] (':' for namespaced targets like
 * Foo::Bar), stop at space/)/newline/quote. Rejects a variable (`${...}`), a
 * quoted string, or a generator expression. Returns 1 on success. */
static int cmake_first_arg(const char *after_paren, char *out, size_t cap)
{
   const char *p = after_paren;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   if (*p == '$' || *p == '"' || *p == '\0' || *p == ')')
      return 0;
   size_t n = 0;
   while (*p && (cmake_ident_char(*p) || *p == '-' || *p == '.' || *p == '+' || *p == ':') &&
          n + 1 < cap)
      out[n++] = *p++;
   out[n] = '\0';
   return n > 0;
}

/* Find every case-insensitive whole-token occurrence of command `kw` (e.g.
 * "project") followed by an opening paren (allowing whitespace/newlines between),
 * and emit cmake_first_arg as an identity of `kind`. Returns the new count;
 * sets *overflow if the per-file cap was hit. */
static int cmake_collect(const char *content, const char *kw, const char *kind,
                         xrepo_identity_t *out, int max, int count, int *overflow)
{
   size_t kwl = strlen(kw);
   for (const char *p = content; *p; p++)
   {
      /* token boundary before kw: start of file or a non-identifier char */
      if (p != content && cmake_ident_char(p[-1]))
         continue;
      if (strncasecmp(p, kw, kwl) != 0)
         continue;
      /* whole-token: the char after kw must not continue an identifier */
      if (cmake_ident_char(p[kwl]))
         continue;
      /* allow whitespace/newlines between the command and its '(' */
      const char *q = p + kwl;
      while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
         q++;
      if (*q != '(')
         continue;
      if (count >= max)
      {
         if (overflow)
            *overflow = 1;
         break;
      }
      char name[256];
      if (cmake_first_arg(q + 1, name, sizeof(name)) && name[0])
      {
         /* de-dup within this file */
         int dup = 0;
         for (int i = 0; i < count; i++)
            if (strcmp(out[i].kind, kind) == 0 && strcmp(out[i].value, name) == 0)
               dup = 1;
         if (!dup)
         {
            snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind);
            snprintf(out[count].value, sizeof(out[count].value), "%s", name);
            count++;
         }
      }
   }
   return count;
}

int xrepo_extract_identities(const char *basename, const char *content, xrepo_identity_t *out,
                             int max, int *overflow)
{
   if (overflow)
      *overflow = 0;
   if (!basename || !content || !out || max <= 0)
      return 0;
   const char *base = path_basename(basename);

   /* Manifest single-id formats reuse the existing pure parser. */
   struct
   {
      const char *file, *kind;
   } single[] = {{"Cargo.toml", "crate"},
                 {"go.mod", "gomod"},
                 {"package.json", "npm"},
                 {"pyproject.toml", "pypi"}};
   for (size_t i = 0; i < sizeof(single) / sizeof(single[0]); i++)
   {
      if (strcmp(base, single[i].file) == 0)
      {
         char id[256];
         if (xrepo_parse_module_id(single[i].file, content, id, sizeof(id)) && id[0])
         {
            snprintf(out[0].kind, sizeof(out[0].kind), "%s", single[i].kind);
            snprintf(out[0].value, sizeof(out[0].value), "%s", id);
            return 1;
         }
         return 0;
      }
   }

   /* CMake: a project() name + every add_library/add_executable target. Command
    * tokens are matched whole, allowing whitespace before '(' (project (Foo)). */
   if (strcmp(base, "CMakeLists.txt") == 0)
   {
      int n = cmake_collect(content, "project", "cmake_project", out, max, 0, overflow);
      n = cmake_collect(content, "add_library", "cmake_target", out, max, n, overflow);
      n = cmake_collect(content, "add_executable", "cmake_target", out, max, n, overflow);
      return n;
   }

   /* pkg-config: the dependency name is the .pc file basename (foo.pc -> foo). */
   if (ends_with(base, ".pc"))
   {
      size_t bl = strlen(base) - 3; /* strip ".pc" */
      if (bl > 0 && bl < sizeof(out[0].value))
      {
         snprintf(out[0].kind, sizeof(out[0].kind), "%s", "pkgconfig");
         memcpy(out[0].value, base, bl);
         out[0].value[bl] = '\0';
         return 1;
      }
   }
   return 0;
}

/* ---- DB builder ---------------------------------------------------------- */

#define CRI_MAX_PER_FILE 256

int db2_cross_repo_rebuild_identities(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CRI_ERRBUF] = "";

   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;

   int ok = 1;
   if (aimee_pg_exec(conn, "DELETE FROM cross_repo_identity", err, sizeof(err)) != 0)
      ok = 0;

   /* One pass over all manifest files across all projects. */
   aimee_pg_stmt_t *sel = NULL;
   if (ok)
   {
      sel = aimee_pg_prepare(
          conn,
          /* f.vendored = 0: a vendored manifest (e.g. third_party/foo/Cargo.toml)
           * declares the VENDORED lib's identity, not the host repo's — claiming it
           * for the host would mis-resolve directives, so exclude it (H0b flag). */
          "SELECT p.name, f.path, fc.content FROM file_contents fc "
          "JOIN files f ON f.id = fc.file_id JOIN projects p ON p.id = f.project_id "
          "WHERE f.vendored = 0 AND (f.path LIKE '%go.mod' OR f.path LIKE '%Cargo.toml' "
          "OR f.path LIKE '%package.json' OR f.path LIKE '%pyproject.toml' "
          "OR f.path LIKE '%CMakeLists.txt' OR f.path LIKE '%.pc')",
          err, sizeof(err));
      if (!sel)
         ok = 0;
   }

   int written = 0;
   aimee_pg_stmt_t *ins = NULL;
   if (ok)
   {
      ins = aimee_pg_prepare(conn,
                             "INSERT INTO cross_repo_identity (project, kind, value) "
                             "VALUES (?1, ?2, ?3) ON CONFLICT (project, kind, value) DO NOTHING",
                             err, sizeof(err));
      if (!ins)
         ok = 0;
   }

   while (ok && aimee_pg_step(sel, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *project = aimee_pg_column_text(sel, 0);
      const char *path = aimee_pg_column_text(sel, 1);
      const char *content = aimee_pg_column_text(sel, 2);
      if (!project || !path || !content)
         continue;
      xrepo_identity_t ids[CRI_MAX_PER_FILE];
      int ovf = 0;
      int n = xrepo_extract_identities(path, content, ids, CRI_MAX_PER_FILE, &ovf);
      if (ovf)
         LOG_WARN(CRI_LOG_TAG, "%s/%s declares > %d identities; truncated", project, path,
                  CRI_MAX_PER_FILE);
      for (int i = 0; i < n; i++)
      {
         aimee_pg_bind_text(ins, "?1", project);
         aimee_pg_bind_text(ins, "?2", ids[i].kind);
         aimee_pg_bind_text(ins, "?3", ids[i].value);
         if (aimee_pg_step(ins, err, sizeof(err)) < 0)
         {
            ok = 0;
            break;
         }
         aimee_pg_reset(ins);
         written++;
      }
   }
   if (sel)
      aimee_pg_finalize(sel);
   if (ins)
      aimee_pg_finalize(ins);

   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_ERROR(CRI_LOG_TAG, "rebuild identities failed: %s", err);
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;
   return written;
}
