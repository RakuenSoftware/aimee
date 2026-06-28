/* cross_repo_build.c: build-declared cross-repo dependency extraction (recall R2).
 * Pure manifest -> declared-dep parsing + a transactional rebuild of the
 * cross_repo_build_dep table from indexed manifest file_contents. See
 * cross_repo_build.h and docs/proposals/pending/cross-repo-recall-recovery.md. */

#include "cross_repo_build.h"

#include "aimee.h"
#include "db2.h"
#include "db_postgres.h"
#include "log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRB_LOG_TAG      "cross_repo.build"
#define CRB_ERRBUF       256
#define CRB_MAX_PER_FILE 128
#define CRB_MAX_PROJECTS 256

/* ---- pure helpers -------------------------------------------------------- */

static const char *crb_basename(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int crb_ends_with(const char *s, const char *suf)
{
   size_t sl = strlen(s), fl = strlen(suf);
   return sl >= fl && strcmp(s + sl - fl, suf) == 0;
}

/* lowercase ASCII into out (truncates at cap). */
static void crb_lower(const char *in, char *out, size_t cap)
{
   size_t i = 0;
   for (; in && in[i] && i + 1 < cap; i++)
      out[i] = (in[i] >= 'A' && in[i] <= 'Z') ? (char)(in[i] + 32) : in[i];
   out[i] = '\0';
}

int xrepo_build_ref_repo(const char *ref, char *out, size_t cap)
{
   if (!ref || !ref[0] || !out || cap == 0)
      return 0;
   /* strip userinfo: scheme://user:tok@host/... -> drop up to and incl. '@' (but only
    * an '@' before the first '/' of the path; scp-form git@host:owner/repo keeps the
    * part after ':'). */
   const char *p = ref;
   const char *at = strchr(p, '@');
   const char *firstslash = strchr(p, '/');
   if (at && (!firstslash || at < firstslash))
      p = at + 1; /* drop user[:tok]@ */
   /* scp form host:owner/repo -> take after the ':' if no '//' scheme */
   if (!strstr(ref, "://"))
   {
      const char *colon = strchr(p, ':');
      if (colon && colon[1])
         p = colon + 1;
   }
   /* trim a trailing slash, then take the last path component. */
   size_t len = strlen(p);
   while (len > 0 && (p[len - 1] == '/' || p[len - 1] == '\n' || p[len - 1] == '\r' ||
                      p[len - 1] == ' ' || p[len - 1] == '"' || p[len - 1] == '\''))
      len--;
   const char *start = p;
   for (size_t i = 0; i < len; i++)
      if (p[i] == '/')
         start = p + i + 1;
   size_t complen = (size_t)((p + len) - start);
   if (complen == 0)
      return 0;
   char tmp[256];
   if (complen >= sizeof(tmp))
      complen = sizeof(tmp) - 1;
   memcpy(tmp, start, complen);
   tmp[complen] = '\0';
   if (crb_ends_with(tmp, ".git"))
      tmp[strlen(tmp) - 4] = '\0';
   if (!tmp[0])
      return 0;
   crb_lower(tmp, out, cap);
   return out[0] ? 1 : 0;
}

/* Read the next whitespace-delimited token at *pp into out (strips surrounding
 * quotes). Advances *pp past it. Returns 1 if a token was read. */
static int crb_next_token(const char **pp, char *out, size_t cap)
{
   const char *p = *pp;
   while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
      p++;
   char quote = 0;
   if (*p == '"' || *p == '\'')
   {
      quote = *p;
      p++;
   }
   size_t n = 0;
   while (*p && n + 1 < cap)
   {
      if (quote && *p == quote)
      {
         p++;
         break;
      }
      if (!quote && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ')'))
         break;
      out[n++] = *p++;
   }
   out[n] = '\0';
   *pp = p;
   return n > 0;
}

static int crb_is_ident(char c)
{
   return isalnum((unsigned char)c) || c == '_';
}

static int crb_add(xrepo_build_dep_t *out, int max, int count, const char *ref, const char *kind,
                   int low, int *overflow)
{
   if (!ref || !ref[0])
      return count;
   for (int i = 0; i < count; i++) /* de-dup within file by (ref,kind) */
      if (strcmp(out[i].ref, ref) == 0 && strcmp(out[i].kind, kind) == 0)
         return count;
   if (count >= max)
   {
      if (overflow)
         *overflow = 1;
      return count;
   }
   snprintf(out[count].ref, sizeof(out[count].ref), "%s", ref);
   snprintf(out[count].kind, sizeof(out[count].kind), "%s", kind);
   out[count].low_conf = low;
   return count + 1;
}

/* CMake / *.cmake: every whole-token GIT_REPOSITORY <url> (FetchContent_Declare,
 * ExternalProject_Add) -> a fetchcontent dep. A ${VAR}/generator-expr url is low_conf.
 * A small state machine skips `#`-to-EOL and `#[[ ... ]]` block comments and "..."
 * string literals, so a commented-out or string-embedded GIT_REPOSITORY is NOT a
 * false dep (roundtable R2b blocker). */
static int crb_cmake(const char *content, xrepo_build_dep_t *out, int max, int count, int *overflow)
{
   const char *kw = "GIT_REPOSITORY";
   size_t kwl = strlen(kw);
   const char *p = content;
   while (*p)
   {
      if (*p == '#')
      {
         if (p[1] == '[' && p[2] == '[') /* #[[ ... ]] block comment */
         {
            const char *e = strstr(p + 3, "]]");
            p = e ? e + 2 : p + strlen(p);
         }
         else /* # line comment */
         {
            while (*p && *p != '\n')
               p++;
         }
         continue;
      }
      if (*p == '"') /* string literal */
      {
         p++;
         while (*p && *p != '"')
            p += (*p == '\\' && p[1]) ? 2 : 1;
         if (*p)
            p++;
         continue;
      }
      if ((p == content || !crb_is_ident(p[-1])) && strncasecmp(p, kw, kwl) == 0 &&
          !crb_is_ident(p[kwl]))
      {
         const char *q = p + kwl;
         char url[512];
         if (crb_next_token(&q, url, sizeof(url)) && url[0])
         {
            int low = (strstr(url, "${") != NULL || strstr(url, "$<") != NULL);
            count = crb_add(out, max, count, url, "fetchcontent", low, overflow);
            p = q;
            continue;
         }
      }
      p++;
   }
   return count;
}

/* .gitmodules: every `url = <url>` -> a submodule dep. */
static int crb_gitmodules(const char *content, xrepo_build_dep_t *out, int max, int count,
                          int *overflow)
{
   for (const char *line = content; line && *line;)
   {
      const char *eol = strchr(line, '\n');
      const char *p = line;
      while (*p == ' ' || *p == '\t')
         p++;
      if (*p == '#' || *p == ';') /* gitconfig comment line */
      {
         line = eol ? eol + 1 : NULL;
         continue;
      }
      if (strncasecmp(p, "url", 3) == 0)
      {
         const char *q = p + 3;
         while (*q == ' ' || *q == '\t')
            q++;
         if (*q == '=')
         {
            q++;
            char url[512];
            const char *qq = q;
            if (crb_next_token(&qq, url, sizeof(url)) && url[0])
            {
               int low = (strstr(url, "${") != NULL);
               count = crb_add(out, max, count, url, "submodule", low, overflow);
            }
         }
      }
      line = eol ? eol + 1 : NULL;
   }
   return count;
}

/* Cargo.toml: `git = "<url>"` and `path = "<rel>"` dependency specs -> manifest deps. */
static int crb_cargo(const char *content, xrepo_build_dep_t *out, int max, int count, int *overflow)
{
   const char *keys[] = {"git", "path"};
   for (const char *line = content; line && *line;)
   {
      const char *eol = strchr(line, '\n');
      size_t llen = eol ? (size_t)(eol - line) : strlen(line);
      char buf[1024];
      size_t cl = llen < sizeof(buf) - 1 ? llen : sizeof(buf) - 1;
      memcpy(buf, line, cl);
      buf[cl] = '\0';
      char *hash = strchr(buf, '#'); /* TOML line comment -> truncate */
      if (hash)
         *hash = '\0';
      for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++)
      {
         char *kp = strstr(buf, keys[k]);
         /* require the key to be token-isolated and followed by '=' (allow ws). */
         while (kp)
         {
            int boundary = (kp == buf) || !crb_is_ident(kp[-1]);
            const char *a = kp + strlen(keys[k]);
            while (*a == ' ' || *a == '\t')
               a++;
            if (boundary && *a == '=')
            {
               a++;
               char val[512];
               const char *aa = a;
               if (crb_next_token(&aa, val, sizeof(val)) && val[0])
               {
                  int low = (strstr(val, "${") != NULL);
                  count = crb_add(out, max, count, val, "manifest", low, overflow);
               }
               break;
            }
            kp = strstr(kp + 1, keys[k]);
         }
      }
      line = eol ? eol + 1 : NULL;
   }
   return count;
}

int xrepo_extract_build_deps(const char *path, const char *content, xrepo_build_dep_t *out, int max,
                             int *overflow)
{
   if (overflow)
      *overflow = 0;
   if (!path || !content || !out || max <= 0)
      return 0;
   const char *base = crb_basename(path);
   if (strcmp(base, "CMakeLists.txt") == 0 || crb_ends_with(base, ".cmake"))
      return crb_cmake(content, out, max, 0, overflow);
   if (strcmp(base, ".gitmodules") == 0)
      return crb_gitmodules(content, out, max, 0, overflow);
   if (strcmp(base, "Cargo.toml") == 0)
      return crb_cargo(content, out, max, 0, overflow);
   return 0;
}

/* ---- DB builder ---------------------------------------------------------- */

typedef struct
{
   char name[128];  /* projects.name */
   char lower[128]; /* lowercased for matching */
} crb_project_t;

/* Map a ref's repo-component to a corpus project name. Returns the projects.name (the
 * canonical-cased name) or NULL. Unique by construction (projects.name is unique), so
 * a repo-component matches 0 or 1 project. */
static const char *crb_map_ref(const char *ref, const crb_project_t *projs, int nproj)
{
   char repo[256];
   if (!xrepo_build_ref_repo(ref, repo, sizeof(repo)))
      return NULL;
   const char *hit = NULL;
   for (int i = 0; i < nproj; i++)
      if (strcmp(projs[i].lower, repo) == 0)
      {
         if (hit) /* >1 corpus repo shares this lowercased basename -> ambiguous, skip */
            return NULL;
         hit = projs[i].name;
      }
   return hit;
}

int db2_cross_repo_rebuild_build_deps(void)
{
   void *conn = db2_conn();
   if (!conn)
      return -1;
   char err[CRB_ERRBUF] = "";

   /* Load the corpus project names once (for ref->repo mapping). */
   crb_project_t projs[CRB_MAX_PROJECTS];
   int nproj = 0;
   {
      aimee_pg_stmt_t *ps =
          aimee_pg_prepare(conn, "SELECT name FROM projects ORDER BY name", err, sizeof(err));
      if (!ps)
         return -1;
      int pstep = AIMEE_PG_DONE;
      while (nproj < CRB_MAX_PROJECTS &&
             (pstep = aimee_pg_step(ps, err, sizeof(err))) == AIMEE_PG_ROW)
      {
         const char *nm = aimee_pg_column_text(ps, 0);
         if (!nm || !nm[0])
            continue;
         snprintf(projs[nproj].name, sizeof(projs[nproj].name), "%s", nm);
         crb_lower(nm, projs[nproj].lower, sizeof(projs[nproj].lower));
         nproj++;
      }
      aimee_pg_finalize(ps);
      /* A mid-cursor error on the projects load would leave a PARTIAL project list,
       * silently dropping build deps to the unloaded repos — fail instead. (A full
       * read terminates at the cap or DONE.) */
      if (pstep != AIMEE_PG_ROW && pstep != AIMEE_PG_DONE)
         return -1;
      if (nproj == CRB_MAX_PROJECTS)
         LOG_WARN(CRB_LOG_TAG,
                  "corpus has >= %d projects; build-dep mapping truncated (tail repos unmappable)",
                  CRB_MAX_PROJECTS);
   }

   if (aimee_pg_exec(conn, "BEGIN", err, sizeof(err)) != 0)
      return -1;
   int ok = (aimee_pg_exec(conn, "DELETE FROM cross_repo_build_dep", err, sizeof(err)) == 0);

   aimee_pg_stmt_t *sel = NULL;
   if (ok)
   {
      /* Non-vendored manifests only (a vendored third_party/ manifest declares the
       * vendored lib's deps, not the host repo's). R2b's top-level-attribution intent
       * (design §2.2): build/_deps are not indexed (R2a code_path_skipped), so the
       * collected manifests are the repo's own. */
      sel = aimee_pg_prepare(
          conn,
          "SELECT p.name, f.path, fc.content FROM file_contents fc "
          "JOIN files f ON f.id = fc.file_id JOIN projects p ON p.id = f.project_id "
          "WHERE f.vendored = 0 AND (f.path LIKE '%CMakeLists.txt' OR f.path LIKE '%.cmake' "
          "OR f.path LIKE '%.gitmodules' OR f.path LIKE '%Cargo.toml') "
          /* defense-in-depth vs R2a's collection skip: never attribute a dep declared
           * in a build-output/dep-cache/worktree subtree to the host repo. */
          "AND f.path NOT LIKE '%/_deps/%' AND f.path NOT LIKE '%/build/%' "
          "AND f.path NOT LIKE '%/.git/%' AND f.path NOT LIKE '%/.aimee/%'",
          err, sizeof(err));
      if (!sel)
         ok = 0;
   }
   aimee_pg_stmt_t *ins = NULL;
   if (ok)
   {
      ins = aimee_pg_prepare(
          conn,
          "INSERT INTO cross_repo_build_dep (caller_project, definer_project, build_kind, "
          "parse_confidence, evidence) VALUES (?1, ?2, ?3, ?4, ?5) "
          "ON CONFLICT (caller_project, definer_project, build_kind, evidence) DO NOTHING",
          err, sizeof(err));
      if (!ins)
         ok = 0;
   }

   int written = 0;
   int step = AIMEE_PG_DONE;
   while (ok && (step = aimee_pg_step(sel, err, sizeof(err))) == AIMEE_PG_ROW)
   {
      const char *caller = aimee_pg_column_text(sel, 0);
      const char *path = aimee_pg_column_text(sel, 1);
      const char *content = aimee_pg_column_text(sel, 2);
      if (!caller || !path || !content)
         continue;
      xrepo_build_dep_t deps[CRB_MAX_PER_FILE];
      int ovf = 0;
      int n = xrepo_extract_build_deps(path, content, deps, CRB_MAX_PER_FILE, &ovf);
      if (ovf)
         LOG_WARN(CRB_LOG_TAG, "%s/%s declares > %d build deps; truncated", caller, path,
                  CRB_MAX_PER_FILE);
      for (int i = 0; i < n; i++)
      {
         const char *definer = crb_map_ref(deps[i].ref, projs, nproj);
         if (!definer || strcmp(definer, caller) == 0) /* external or self */
            continue;
         aimee_pg_bind_text(ins, "?1", caller);
         aimee_pg_bind_text(ins, "?2", definer);
         aimee_pg_bind_text(ins, "?3", deps[i].kind);
         aimee_pg_bind_text(ins, "?4", deps[i].low_conf ? "low" : "high");
         aimee_pg_bind_text(ins, "?5", deps[i].ref);
         if (aimee_pg_step(ins, err, sizeof(err)) < 0)
         {
            ok = 0;
            break;
         }
         aimee_pg_reset(ins);
         written++;
      }
   }
   /* A mid-cursor step error (negative / not the clean terminal) must NOT fall
    * through to COMMIT with a partial table (roundtable R2b blocker). */
   if (ok && step != AIMEE_PG_ROW && step != AIMEE_PG_DONE)
      ok = 0;
   if (sel)
      aimee_pg_finalize(sel);
   if (ins)
      aimee_pg_finalize(ins);

   if (!ok)
   {
      (void)aimee_pg_exec(conn, "ROLLBACK", err, sizeof(err));
      LOG_ERROR(CRB_LOG_TAG, "rebuild build_deps failed: %s", err);
      return -1;
   }
   if (aimee_pg_exec(conn, "COMMIT", err, sizeof(err)) != 0)
      return -1;
   return written;
}
