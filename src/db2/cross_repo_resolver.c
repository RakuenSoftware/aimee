/* cross_repo_resolver.c: pure, DB-free core of the cross-repo dependency
 * resolver (S2a: import resolution + distinctiveness). See cross_repo_resolver.h
 * and docs/proposals/pending/cross-repo-dependency-graph.md §3.1/§3.3/§3.7. */

#include "cross_repo_resolver.h"

#include <stdint.h>
#include <string.h>

/* ---- language classification --------------------------------------------- */

static int path_has_ext(const char *path, const char *ext)
{
   size_t pl = strlen(path), el = strlen(ext);
   return pl >= el && strcmp(path + pl - el, ext) == 0;
}

xrepo_lang_t xrepo_lang_from_path(const char *path)
{
   if (!path || !path[0])
      return XREPO_LANG_UNKNOWN;
   /* C++ first: its extensions are unambiguous; .h is treated as C. */
   if (path_has_ext(path, ".cpp") || path_has_ext(path, ".cc") || path_has_ext(path, ".cxx") ||
       path_has_ext(path, ".hpp") || path_has_ext(path, ".hh") || path_has_ext(path, ".hxx"))
      return XREPO_LANG_CPP;
   if (path_has_ext(path, ".c") || path_has_ext(path, ".h"))
      return XREPO_LANG_C;
   if (path_has_ext(path, ".rs"))
      return XREPO_LANG_RUST;
   if (path_has_ext(path, ".go"))
      return XREPO_LANG_GO;
   if (path_has_ext(path, ".tsx") || path_has_ext(path, ".ts"))
      return XREPO_LANG_TS;
   if (path_has_ext(path, ".jsx") || path_has_ext(path, ".js") || path_has_ext(path, ".mjs") ||
       path_has_ext(path, ".cjs"))
      return XREPO_LANG_JS;
   if (path_has_ext(path, ".py"))
      return XREPO_LANG_PYTHON;
   return XREPO_LANG_UNKNOWN;
}

const char *xrepo_lang_name(xrepo_lang_t lang)
{
   switch (lang)
   {
   case XREPO_LANG_C:
      return "c";
   case XREPO_LANG_CPP:
      return "cpp";
   case XREPO_LANG_RUST:
      return "rust";
   case XREPO_LANG_GO:
      return "go";
   case XREPO_LANG_TS:
      return "ts";
   case XREPO_LANG_JS:
      return "js";
   case XREPO_LANG_PYTHON:
      return "python";
   default:
      return "unknown";
   }
}

/* ---- vendored / third-party path classification (H0b) -------------------- */

/* A definition under a vendored/third-party subtree is not first-party API of
 * its repo; §4 of the precision-hardening proposal prefers a non-vendored definer
 * and routes vendored-only collisions to AMBIGUOUS. Matches a whole path SEGMENT
 * (so "vendored_thing/x.c" is NOT vendored but "vendor/x.c" is). Returns 1/0. */
int xrepo_path_is_vendored(const char *path)
{
   if (!path || !path[0])
      return 0;
   static const char *dirs[] = {
       "vendor",      "third_party", "third-party",   "extern",
       "external",    "deps",        ".deps",         "_deps", /* CMake FetchContent cache */
       "subprojects", "Pods",        "node_modules",  "bower_components",
       ".venv",       "venv",        "site-packages", NULL};
   const char *seg = path;
   while (seg && *seg)
   {
      const char *slash = strchr(seg, '/');
      size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
      for (int i = 0; dirs[i]; i++)
         if (strlen(dirs[i]) == len && strncmp(seg, dirs[i], len) == 0)
            return 1;
      seg = slash ? slash + 1 : NULL;
   }
   return 0;
}

/* ---- distinctiveness (§3.3) ---------------------------------------------- */

size_t xrepo_utf8_len(const char *s)
{
   size_t n = 0;
   if (!s)
      return 0;
   for (const unsigned char *p = (const unsigned char *)s; *p; p++)
      if ((*p & 0xC0) != 0x80) /* count lead bytes, skip UTF-8 continuation bytes */
         n++;
   return n;
}

int xrepo_name_distinctive(const char *symbol, const xrepo_distinct_stats_t *stats,
                           const xrepo_distinct_cfg_t *cfg)
{
   if (!symbol || !stats || !cfg)
      return 0;
   if ((int)xrepo_utf8_len(symbol) < cfg->len_min)
      return 0;
   if (cfg->k > 0 && stats->callee_repo_count >= cfg->k)
      return 0;
   if (cfg->m > 0 && stats->definer_repo_count >= cfg->m)
      return 0;
   if (cfg->p_pct > 0 && stats->caller_file_pct >= cfg->p_pct)
      return 0;
   return 1;
}

/* ---- import resolution (§3.7) -------------------------------------------- */

/* Builtin C/C++ system/framework headers: an include resolving to one of these
 * is rejected (never a cross-repo edge), even if a repo happens to index a file
 * of the same basename. The set is intentionally conservative; the configurable
 * per-workspace blocklist (S3) extends it, and a stale entry degrades to
 * AMBIGUOUS rather than failing open/closed. */
static const char *const C_SYSTEM_HEADERS[] = {
    "stdio.h",     "stdlib.h",      "string.h",      "stddef.h", "stdint.h",
    "stdbool.h",   "stdarg.h",      "ctype.h",       "errno.h",  "math.h",
    "time.h",      "assert.h",      "limits.h",      "unistd.h", "fcntl.h",
    "signal.h",    "memory",        "vector",        "string",   "string_view",
    "cstdio",      "cstdlib",       "cstring",       "cstdint",  "cstddef",
    "cstdarg",     "cerrno",        "cctype",        "climits",  "map",
    "set",         "unordered_map", "unordered_set", "list",     "deque",
    "queue",       "stack",         "algorithm",     "numeric",  "ranges",
    "utility",     "tuple",         "iostream",      "sstream",  "fstream",
    "iomanip",     "mutex",         "thread",        "atomic",   "condition_variable",
    "future",      "chrono",        "filesystem",    "cassert",  "cmath",
    "stdexcept",   "array",         "functional",    "optional", "variant",
    "type_traits", "memory.h",      "pthread.h",     "dlfcn.h",
};

static const char *basename_of(const char *path)
{
   const char *slash = strrchr(path, '/');
   return slash ? slash + 1 : path;
}

static int is_c_system_header(const char *inc)
{
   const char *base = basename_of(inc);
   for (size_t i = 0; i < sizeof(C_SYSTEM_HEADERS) / sizeof(C_SYSTEM_HEADERS[0]); i++)
      if (strcmp(base, C_SYSTEM_HEADERS[i]) == 0)
         return 1;
   return 0;
}

/* True if `path` ends with `suffix` on a path-component boundary
 * (path == suffix, or path ends with "/"+suffix). */
static int path_suffix_match(const char *path, const char *suffix)
{
   size_t pl = strlen(path), sl = strlen(suffix);
   if (sl == 0 || pl < sl)
      return 0;
   if (strcmp(path + pl - sl, suffix) != 0)
      return 0;
   return pl == sl || path[pl - sl - 1] == '/';
}

static int is_caller(const xrepo_repo_desc_t *d, const char *caller_repo)
{
   return caller_repo && d->name && strcmp(d->name, caller_repo) == 0;
}

/* Turn a list of matched definer-repo indices into a result. count==0 -> NONE,
 * 1 -> ONE, >1 -> MANY with the colliders enumerated (capped, total in
 * collision_count). The caller is assumed already excluded from `idx`. */
static xrepo_resolve_result_t result_from_matches(const int *idx, int count, int sys,
                                                  xrepo_import_modality_t modality)
{
   xrepo_resolve_result_t r;
   memset(&r, 0, sizeof(r));
   r.system_header = sys;
   r.modality = modality;
   r.repo_index = -1;
   if (count <= 0)
   {
      r.cardinality = XREPO_RESOLVE_NONE;
      return r;
   }
   if (count == 1)
   {
      r.cardinality = XREPO_RESOLVE_ONE;
      r.repo_index = idx[0];
      return r;
   }
   r.cardinality = XREPO_RESOLVE_MANY;
   r.collision_count = count;
   for (int i = 0; i < count && i < XREPO_MAX_COLLISIONS; i++)
      r.collisions[i] = idx[i];
   return r;
}

/* C/C++: path-suffix of the include resolving to indexed headers. A suffix
 * matching headers in exactly one trusted repo -> ONE; in several -> MANY (the
 * vendored/header-only collision case). File-level multiplicity within one repo
 * still resolves to that one repo. The caller repo is excluded (no self-edge). */
static xrepo_resolve_result_t resolve_c(const char *inc, const char *caller_repo,
                                        xrepo_import_modality_t modality,
                                        const xrepo_repo_desc_t *descs, size_t n)
{
   if (is_c_system_header(inc))
      return result_from_matches(NULL, 0, 1, modality);

   int matches[XREPO_MAX_COLLISIONS];
   int count = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (!descs[i].trusted || !descs[i].headers || is_caller(&descs[i], caller_repo))
         continue;
      int repo_hit = 0;
      for (size_t h = 0; h < descs[i].header_count && !repo_hit; h++)
         if (descs[i].headers[h] && path_suffix_match(descs[i].headers[h], inc))
            repo_hit = 1;
      if (repo_hit)
      {
         if (count < XREPO_MAX_COLLISIONS)
            matches[count] = (int)i;
         count++;
      }
   }
   return result_from_matches(matches, count, 0, modality);
}

/* Copy the leading component of `s` up to any char in `seps` into out[]. */
static void leading_component(const char *s, const char *seps, char *out, size_t cap)
{
   size_t i = 0;
   for (; s[i] && i + 1 < cap && !strchr(seps, s[i]); i++)
      out[i] = s[i];
   out[i] = '\0';
}

/* Rust/TS/JS/Python: match the import's leading package/crate token to a repo's
 * module_id (exact). MANY only on a genuine module_id collision. Caller excluded. */
static xrepo_resolve_result_t resolve_by_token(const char *token, const char *caller_repo,
                                               xrepo_import_modality_t modality,
                                               const xrepo_repo_desc_t *descs, size_t n)
{
   if (!token[0])
      return result_from_matches(NULL, 0, 0, modality);
   int matches[XREPO_MAX_COLLISIONS];
   int count = 0;
   for (size_t i = 0; i < n; i++)
   {
      if (is_caller(&descs[i], caller_repo))
         continue;
      if (descs[i].module_id && descs[i].module_id[0] && strcmp(descs[i].module_id, token) == 0)
      {
         if (count < XREPO_MAX_COLLISIONS)
            matches[count] = (int)i;
         count++;
      }
   }
   return result_from_matches(matches, count, 0, modality);
}

/* Go: import path matched to the longest module_id that is a prefix of it (so a
 * monorepo sub-package resolves to its containing module). Equal-length distinct
 * module_id prefixes -> MANY. Caller excluded (no self-edge). */
static xrepo_resolve_result_t resolve_go(const char *imp, const char *caller_repo,
                                         xrepo_import_modality_t modality,
                                         const xrepo_repo_desc_t *descs, size_t n)
{
   size_t best_len = 0;
   int matches[XREPO_MAX_COLLISIONS];
   int count = 0;
   for (size_t i = 0; i < n; i++)
   {
      const char *m = descs[i].module_id;
      if (!m || !m[0] || is_caller(&descs[i], caller_repo))
         continue;
      size_t ml = strlen(m);
      int prefix = strcmp(imp, m) == 0 || (strncmp(imp, m, ml) == 0 && imp[ml] == '/');
      if (!prefix)
         continue;
      if (ml > best_len)
      {
         best_len = ml; /* a strictly longer module wins -> reset the match set */
         count = 0;
         matches[count++] = (int)i;
      }
      else if (ml == best_len)
      {
         if (count < XREPO_MAX_COLLISIONS)
            matches[count] = (int)i;
         count++;
      }
   }
   return result_from_matches(matches, count, 0, modality);
}

xrepo_resolve_result_t xrepo_resolve_import_to_repo(const char *raw_import, xrepo_lang_t lang,
                                                    const char *caller_repo,
                                                    xrepo_import_modality_t modality,
                                                    const xrepo_repo_desc_t *descs,
                                                    size_t desc_count)
{
   xrepo_resolve_result_t none = result_from_matches(NULL, 0, 0, modality);
   if (!raw_import || !raw_import[0] || !descs)
      return none;

   char tok[256];
   switch (lang)
   {
   case XREPO_LANG_C:
   case XREPO_LANG_CPP:
      return resolve_c(raw_import, caller_repo, modality, descs, desc_count);

   case XREPO_LANG_GO:
      return resolve_go(raw_import, caller_repo, modality, descs, desc_count);

   case XREPO_LANG_RUST:
   {
      const char *rs = raw_import;
      while (*rs == ':') /* Rust 2018+ absolute path "::crate::..." */
         rs++;
      leading_component(rs, ":", tok, sizeof(tok)); /* "crate::path" -> "crate" */
      if (strcmp(tok, "crate") == 0 || strcmp(tok, "super") == 0 || strcmp(tok, "self") == 0)
         return none; /* intra-repo path */
      return resolve_by_token(tok, caller_repo, modality, descs, desc_count);
   }

   case XREPO_LANG_PYTHON:
      if (raw_import[0] == '.') /* relative import */
         return none;
      leading_component(raw_import, ".", tok, sizeof(tok)); /* "pkg.sub" -> "pkg" */
      return resolve_by_token(tok, caller_repo, modality, descs, desc_count);

   case XREPO_LANG_TS:
   case XREPO_LANG_JS:
      if (raw_import[0] == '.' || raw_import[0] == '/') /* relative / absolute path */
         return none;
      if (raw_import[0] == '@')
      {
         /* scoped package: "@scope/name[/sub]" -> "@scope/name" */
         const char *slash = strchr(raw_import, '/');
         const char *slash2 = slash ? strchr(slash + 1, '/') : NULL;
         size_t len = slash2 ? (size_t)(slash2 - raw_import)
                             : (slash ? strlen(raw_import) : strlen(raw_import));
         if (len + 1 > sizeof(tok))
            len = sizeof(tok) - 1;
         memcpy(tok, raw_import, len);
         tok[len] = '\0';
      }
      else
      {
         leading_component(raw_import, "/", tok, sizeof(tok)); /* "pkg/sub" -> "pkg" */
      }
      return resolve_by_token(tok, caller_repo, modality, descs, desc_count);

   default:
      return none; /* UNKNOWN -> import route unavailable (UNIMPLEMENTED surfaced upstream) */
   }
}
