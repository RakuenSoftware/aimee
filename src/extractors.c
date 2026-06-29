/* extractors.c: source code parsing, definition extraction (functions, classes, structs) */
#include "aimee.h"
#include "extractors_extra.h"
#include "headers/code_treesitter.h" /* §2 tree-sitter front-end (fallback to below) */
#include <ctype.h>

/* --- Extension mapping --- */

static const char *js_exts[] = {".js", ".mjs", ".cjs", NULL};
static const char *ts_exts[] = {".ts", ".tsx", NULL};
static const char *py_exts[] = {".py", NULL};
static const char *go_exts[] = {".go", NULL};
static const char *cs_exts[] = {".cs", NULL};
static const char *sh_exts[] = {".sh", ".bash", ".zsh", NULL};
static const char *css_exts[] = {".css", NULL};
static const char *dart_exts[] = {".dart", NULL};
static const char *c_exts[] = {".c",   ".h",  ".cpp", ".cc",  ".cxx",
                               ".hpp", ".hh", ".hxx", ".inc", NULL};
static const char *lua_exts[] = {".lua", NULL};
static const char *java_exts[] = {".java", NULL};
static const char *rust_exts[] = {".rs", NULL};
static const char *ruby_exts[] = {".rb", NULL};
static const char *kotlin_exts[] = {".kt", ".kts", NULL};
static const char *swift_exts[] = {".swift", NULL};
static const char *php_exts[] = {".php", NULL};

static int ext_matches(const char *ext, const char **list)
{
   for (int i = 0; list[i]; i++)
   {
      if (strcmp(ext, list[i]) == 0)
         return 1;
   }
   return 0;
}

int index_has_extractor(const char *ext)
{
   if (!ext || !*ext)
      return 0;
   return ext_matches(ext, js_exts) || ext_matches(ext, ts_exts) || ext_matches(ext, py_exts) ||
          ext_matches(ext, go_exts) || ext_matches(ext, cs_exts) || ext_matches(ext, sh_exts) ||
          ext_matches(ext, css_exts) || ext_matches(ext, dart_exts) || ext_matches(ext, c_exts) ||
          ext_matches(ext, lua_exts) || ext_matches(ext, java_exts) ||
          ext_matches(ext, rust_exts) || ext_matches(ext, ruby_exts) ||
          ext_matches(ext, kotlin_exts) || ext_matches(ext, swift_exts) ||
          ext_matches(ext, php_exts);
}

/* --- Utility: line iteration --- */

void for_each_line(const char *content, line_fn fn, void *ctx)
{
   const char *p = content;
   int lineno = 1;
   while (*p)
   {
      const char *eol = strchr(p, '\n');
      size_t len = eol ? (size_t)(eol - p) : strlen(p);

      /* Use a stack buffer for the line */
      char lbuf[4096];
      if (len >= sizeof(lbuf))
         len = sizeof(lbuf) - 1;
      memcpy(lbuf, p, len);
      lbuf[len] = '\0';

      fn(lbuf, lineno, ctx);
      lineno++;

      if (!eol)
         break;
      p = eol + 1;
   }
}

/* --- Utility: add a string to output array --- */

int add_str(char **out, int count, int max, const char *s)
{
   if (count >= max)
      return count;
   out[count] = strdup(s);
   return out[count] ? count + 1 : count;
}

int add_def(definition_t *out, int count, int max, const char *name, const char *kind, int line)
{
   if (count >= max)
      return count;
   size_t nlen = strlen(name);
   if (nlen >= sizeof(out[count].name))
      nlen = sizeof(out[count].name) - 1;
   memcpy(out[count].name, name, nlen);
   out[count].name[nlen] = '\0';
   size_t klen = strlen(kind);
   if (klen >= sizeof(out[count].kind))
      klen = sizeof(out[count].kind) - 1;
   memcpy(out[count].kind, kind, klen);
   out[count].kind[klen] = '\0';
   out[count].line = line;
   return count + 1;
}

int add_call(call_ref_t *out, int count, int max, const char *caller, const char *callee, int line)
{
   if (count >= max)
      return count;
   size_t clen = strlen(caller);
   if (clen >= sizeof(out[count].caller))
      clen = sizeof(out[count].caller) - 1;
   memcpy(out[count].caller, caller, clen);
   out[count].caller[clen] = '\0';
   size_t elen = strlen(callee);
   if (elen >= sizeof(out[count].callee))
      elen = sizeof(out[count].callee) - 1;
   memcpy(out[count].callee, callee, elen);
   out[count].callee[elen] = '\0';
   out[count].line = line;
   return count + 1;
}

/* --- Utility: extract quoted string after a position --- */

const char *extract_quoted(const char *p, char *buf, size_t len)
{
   char q = *p;
   if (q != '\'' && q != '"')
      return NULL;
   p++;
   const char *end = strchr(p, q);
   if (!end)
      return NULL;
   size_t slen = (size_t)(end - p);
   if (slen >= len)
      slen = len - 1;
   memcpy(buf, p, slen);
   buf[slen] = '\0';
   return buf;
}

/* Skip leading whitespace, return pointer */
const char *skip_ws(const char *s)
{
   while (*s && isspace((unsigned char)*s))
      s++;
   return s;
}

/* Extract identifier starting at p */
const char *extract_ident(const char *p, char *buf, size_t len)
{
   size_t i = 0;
   while (i < len - 1 && (isalnum((unsigned char)p[i]) || p[i] == '_'))
   {
      buf[i] = p[i];
      i++;
   }
   buf[i] = '\0';
   return i > 0 ? buf : NULL;
}

/* --- JavaScript / TypeScript imports --- */

static void js_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p;

   (void)lineno;

   /* require('...') */
   p = strstr(line, "require(");
   if (p)
   {
      p += 8; /* skip require( */
      char buf[512];
      if (extract_quoted(p, buf, sizeof(buf)) && buf[0] == '.')
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
      return;
   }

   /* import ... from '...' */
   p = strstr(line, "from ");
   if (p && strstr(line, "import"))
   {
      p += 5;
      p = skip_ws(p);
      char buf[512];
      if (extract_quoted(p, buf, sizeof(buf)) && buf[0] == '.')
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
   }
}

/* --- JavaScript / TypeScript exports --- */

static void js_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p;

   (void)lineno;

   if (strstr(line, "module.exports"))
   {
      ec->count = add_str(ec->out, ec->count, ec->max, "module.exports");
      return;
   }

   p = strstr(line, "export ");
   if (!p)
      return;
   p += 7;
   p = skip_ws(p);

   static const char *kw[] = {"function ", "class ",   "const ",          "let ",
                              "var ",      "default ", "async function ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      if (strncmp(p, kw[i], strlen(kw[i])) == 0)
      {
         const char *np = p + strlen(kw[i]);
         np = skip_ws(np);
         char name[256];
         if (extract_ident(np, name, sizeof(name)))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }
}

/* --- TypeScript extras (interface, type, enum) --- */

static void ts_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p;

   /* First check standard JS exports */
   js_export_line(line, lineno, ctx);

   p = strstr(line, "export ");
   if (!p)
      return;
   p += 7;
   p = skip_ws(p);

   static const char *ts_kw[] = {"interface ", "type ", "enum ", NULL};

   for (int i = 0; ts_kw[i]; i++)
   {
      if (strncmp(p, ts_kw[i], strlen(ts_kw[i])) == 0)
      {
         const char *np = p + strlen(ts_kw[i]);
         np = skip_ws(np);
         char name[256];
         if (extract_ident(np, name, sizeof(name)))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }
}

/* --- JavaScript routes --- */

static void js_route_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *rc = (export_ctx_t *)ctx;
   (void)lineno;

   static const char *methods[] = {"app.get(",    "app.post(",      "app.put(",
                                   "app.delete(", "router.get(",    "router.post(",
                                   "router.put(", "router.delete(", NULL};

   for (int i = 0; methods[i]; i++)
   {
      const char *p = strstr(line, methods[i]);
      if (p)
      {
         p += strlen(methods[i]);
         p = skip_ws(p);
         char buf[512];
         if (extract_quoted(p, buf, sizeof(buf)))
            rc->count = add_str(rc->out, rc->count, rc->max, buf);
         return;
      }
   }
}

/* --- JavaScript/TypeScript definitions --- */

static void js_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   /* function name( */
   if (strncmp(p, "function ", 9) == 0)
   {
      char name[256];
      if (extract_ident(p + 9, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }

   /* async function name( */
   if (strncmp(p, "async function ", 15) == 0)
   {
      char name[256];
      if (extract_ident(p + 15, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }

   /* class Name */
   if (strncmp(p, "class ", 6) == 0)
   {
      char name[256];
      if (extract_ident(p + 6, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }

   /* TypeScript: interface, type, enum at top level */
   if (dc->is_ts)
   {
      if (strncmp(p, "interface ", 10) == 0)
      {
         char name[256];
         if (extract_ident(p + 10, name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
      if (strncmp(p, "type ", 5) == 0)
      {
         char name[256];
         if (extract_ident(p + 5, name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
      if (strncmp(p, "enum ", 5) == 0)
      {
         char name[256];
         if (extract_ident(p + 5, name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
   }
}

/* --- Python --- */

static void py_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* from . import ... or from .module import ... */
   if (strncmp(p, "from ", 5) == 0)
   {
      p += 5;
      p = skip_ws(p);
      if (*p == '.')
      {
         /* Relative import */
         const char *start = p;
         while (*p == '.')
            p++;
         /* Get module name if any */
         char buf[512];
         size_t len = 0;
         while (isalnum((unsigned char)p[len]) || p[len] == '_' || p[len] == '.')
            len++;
         size_t total = (size_t)(p - start) + len;
         if (total > 0 && total < sizeof(buf))
         {
            memcpy(buf, start, total);
            buf[total] = '\0';
            ic->count = add_str(ic->out, ic->count, ic->max, buf);
         }
      }
   }
}

static void py_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;

   (void)lineno;

   /* Only top-level (no leading whitespace) */
   if (line[0] == ' ' || line[0] == '\t')
      return;

   if (strncmp(line, "def ", 4) == 0)
   {
      char name[256];
      if (extract_ident(line + 4, name, sizeof(name)))
         ec->count = add_str(ec->out, ec->count, ec->max, name);
      return;
   }
   if (strncmp(line, "class ", 6) == 0)
   {
      char name[256];
      if (extract_ident(line + 6, name, sizeof(name)))
         ec->count = add_str(ec->out, ec->count, ec->max, name);
   }
}

static void py_route_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *rc = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   static const char *decorators[] = {"@app.route(",  "@app.get(",    "@app.post(",    "@app.put(",
                                      "@app.delete(", "@router.get(", "@router.post(", NULL};

   for (int i = 0; decorators[i]; i++)
   {
      if (strncmp(p, decorators[i], strlen(decorators[i])) == 0)
      {
         const char *q = p + strlen(decorators[i]);
         q = skip_ws(q);
         char buf[512];
         if (extract_quoted(q, buf, sizeof(buf)))
            rc->count = add_str(rc->out, rc->count, rc->max, buf);
         return;
      }
   }
}

static void py_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;

   /* Only top-level definitions */
   if (line[0] == ' ' || line[0] == '\t')
      return;

   if (strncmp(line, "def ", 4) == 0)
   {
      char name[256];
      if (extract_ident(line + 4, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }
   if (strncmp(line, "class ", 6) == 0)
   {
      char name[256];
      if (extract_ident(line + 6, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
   }
}

/* --- Go --- */

static void go_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* import "..." or bare "..." inside import block */
   if (strncmp(p, "import ", 7) == 0)
      p += 7;
   p = skip_ws(p);

   char buf[512];
   if (extract_quoted(p, buf, sizeof(buf)))
      ic->count = add_str(ic->out, ic->count, ic->max, buf);
}

static void go_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   static const char *kw[] = {"func ", "type ", "var ", "const ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      if (strncmp(p, kw[i], strlen(kw[i])) == 0)
      {
         const char *np = p + strlen(kw[i]);
         /* Skip receiver for func: (r *Receiver) */
         if (i == 0 && *np == '(')
         {
            np = strchr(np, ')');
            if (!np)
               return;
            np++;
            np = skip_ws(np);
         }
         char name[256];
         if (extract_ident(np, name, sizeof(name)) && isupper((unsigned char)name[0]))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }
}

static void go_route_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *rc = (export_ctx_t *)ctx;
   (void)lineno;

   static const char *patterns[] = {"HandleFunc(", "Handle(",  ".Get(",   ".Post(",
                                    ".Put(",       ".Delete(", ".Patch(", NULL};

   for (int i = 0; patterns[i]; i++)
   {
      const char *p = strstr(line, patterns[i]);
      if (p)
      {
         p += strlen(patterns[i]);
         p = skip_ws(p);
         char buf[512];
         if (extract_quoted(p, buf, sizeof(buf)))
            rc->count = add_str(rc->out, rc->count, rc->max, buf);
         return;
      }
   }
}

static void go_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   static const char *kw[] = {"func ", "type ", "const ", "var ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      if (strncmp(p, kw[i], strlen(kw[i])) == 0)
      {
         const char *np = p + strlen(kw[i]);
         /* Skip receiver */
         if (i == 0 && *np == '(')
         {
            np = strchr(np, ')');
            if (!np)
               return;
            np++;
            np = skip_ws(np);
         }
         char name[256];
         if (extract_ident(np, name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
   }
}

/* --- Dispatch tables --- */

typedef enum
{
   LANG_JS = 0,
   LANG_TS,
   LANG_PY,
   LANG_GO,
   LANG_CS,
   LANG_SH,
   LANG_CSS,
   LANG_DART,
   LANG_C,
   LANG_LUA,
   LANG_JAVA,
   LANG_RUST,
   LANG_RUBY,
   LANG_KOTLIN,
   LANG_SWIFT,
   LANG_PHP,
   LANG_UNKNOWN
} lang_t;

static lang_t detect_lang(const char *ext)
{
   if (ext_matches(ext, js_exts))
      return LANG_JS;
   if (ext_matches(ext, ts_exts))
      return LANG_TS;
   if (ext_matches(ext, py_exts))
      return LANG_PY;
   if (ext_matches(ext, go_exts))
      return LANG_GO;
   if (ext_matches(ext, cs_exts))
      return LANG_CS;
   if (ext_matches(ext, sh_exts))
      return LANG_SH;
   if (ext_matches(ext, css_exts))
      return LANG_CSS;
   if (ext_matches(ext, dart_exts))
      return LANG_DART;
   if (ext_matches(ext, c_exts))
      return LANG_C;
   if (ext_matches(ext, lua_exts))
      return LANG_LUA;
   if (ext_matches(ext, java_exts))
      return LANG_JAVA;
   if (ext_matches(ext, rust_exts))
      return LANG_RUST;
   if (ext_matches(ext, ruby_exts))
      return LANG_RUBY;
   if (ext_matches(ext, kotlin_exts))
      return LANG_KOTLIN;
   if (ext_matches(ext, swift_exts))
      return LANG_SWIFT;
   if (ext_matches(ext, php_exts))
      return LANG_PHP;
   return LANG_UNKNOWN;
}

int extract_imports(const char *ext, const char *content, char **out, int max)
{
   return extract_imports_sys(ext, content, out, NULL, max);
}

int extract_imports_sys(const char *ext, const char *content, char **out, int *sys, int max)
{
   lang_t lang = detect_lang(ext);
   import_ctx_t ctx = {out, 0, max, lang == LANG_TS, sys};
   if (sys)
      for (int i = 0; i < max; i++)
         sys[i] = 0; /* default: not a system/angle include */

   switch (lang)
   {
   case LANG_JS:
   case LANG_TS:
      for_each_line(content, js_import_line, &ctx);
      break;
   case LANG_PY:
      for_each_line(content, py_import_line, &ctx);
      break;
   case LANG_GO:
      for_each_line(content, go_import_line, &ctx);
      break;
   case LANG_CS:
      for_each_line(content, cs_import_line, &ctx);
      break;
   case LANG_SH:
      for_each_line(content, sh_import_line, &ctx);
      break;
   case LANG_CSS:
      for_each_line(content, css_import_line, &ctx);
      break;
   case LANG_DART:
      for_each_line(content, dart_import_line, &ctx);
      break;
   case LANG_C:
      for_each_line(content, c_import_line, &ctx);
      break;
   case LANG_LUA:
      for_each_line(content, lua_import_line, &ctx);
      break;
   case LANG_JAVA:
      for_each_line(content, java_import_line, &ctx);
      break;
   case LANG_RUST:
      for_each_line(content, rust_import_line, &ctx);
      break;
   case LANG_RUBY:
      for_each_line(content, ruby_import_line, &ctx);
      break;
   case LANG_KOTLIN:
      for_each_line(content, kotlin_import_line, &ctx);
      break;
   case LANG_SWIFT:
      for_each_line(content, swift_import_line, &ctx);
      break;
   case LANG_PHP:
      for_each_line(content, php_import_line, &ctx);
      break;
   default:
      break;
   }

   return ctx.count;
}

int extract_exports(const char *ext, const char *content, char **out, int max)
{
   lang_t lang = detect_lang(ext);
   export_ctx_t ctx = {out, 0, max};

   switch (lang)
   {
   case LANG_JS:
      for_each_line(content, js_export_line, &ctx);
      break;
   case LANG_TS:
      for_each_line(content, ts_export_line, &ctx);
      break;
   case LANG_PY:
      for_each_line(content, py_export_line, &ctx);
      break;
   case LANG_GO:
      for_each_line(content, go_export_line, &ctx);
      break;
   case LANG_CS:
      for_each_line(content, cs_export_line, &ctx);
      break;
   case LANG_CSS:
      for_each_line(content, css_export_line, &ctx);
      break;
   case LANG_DART:
      for_each_line(content, dart_export_line, &ctx);
      break;
   case LANG_C:
      for_each_line(content, c_export_line, &ctx);
      break;
   case LANG_LUA:
      for_each_line(content, lua_export_line, &ctx);
      break;
   case LANG_JAVA:
      for_each_line(content, java_export_line, &ctx);
      break;
   case LANG_RUST:
      for_each_line(content, rust_export_line, &ctx);
      break;
   case LANG_RUBY:
      for_each_line(content, ruby_export_line, &ctx);
      break;
   case LANG_KOTLIN:
      for_each_line(content, kotlin_export_line, &ctx);
      break;
   case LANG_SWIFT:
      for_each_line(content, swift_export_line, &ctx);
      break;
   case LANG_PHP:
      for_each_line(content, php_export_line, &ctx);
      break;
   default:
      break;
   }

   return ctx.count;
}

int extract_routes(const char *ext, const char *content, char **out, int max)
{
   lang_t lang = detect_lang(ext);
   export_ctx_t ctx = {out, 0, max};

   switch (lang)
   {
   case LANG_JS:
   case LANG_TS:
      for_each_line(content, js_route_line, &ctx);
      break;
   case LANG_PY:
      for_each_line(content, py_route_line, &ctx);
      break;
   case LANG_GO:
      for_each_line(content, go_route_line, &ctx);
      break;
   case LANG_CS:
      for_each_line(content, cs_route_line, &ctx);
      break;
   case LANG_JAVA:
      for_each_line(content, java_route_line, &ctx);
      break;
   case LANG_RUBY:
      for_each_line(content, ruby_route_line, &ctx);
      break;
   case LANG_KOTLIN:
      for_each_line(content, kotlin_route_line, &ctx);
      break;
   default:
      break;
   }

   return ctx.count;
}

int code_def_end_line(const char *content, int start_line, const char *ext)
{
   if (!content || start_line < 1)
      return start_line < 1 ? 1 : start_line;
   lang_t lang = detect_lang(ext);

   /* Walk to the start of line `start_line` (1-based). */
   const char *p = content;
   int line = 1;
   while (line < start_line && *p)
   {
      if (*p == '\n')
         line++;
      p++;
   }
   if (!*p)
      return start_line;

   if (lang == LANG_PY)
   {
      /* Body = the run of following lines indented deeper than the def line;
       * blank lines don't end it and trailing blanks aren't included. */
      int def_indent = 0;
      const char *q = p;
      while (*q == ' ' || *q == '\t')
      {
         def_indent++;
         q++;
      }
      int end = start_line, cur = start_line;
      const char *r = p;
      while (*r && *r != '\n')
         r++;
      while (*r == '\n')
      {
         r++;
         cur++;
         if (!*r)
            break;
         const char *s = r;
         int indent = 0;
         while (*s == ' ' || *s == '\t')
         {
            indent++;
            s++;
         }
         int blank = (*s == '\n' || *s == '\r' || *s == '\0');
         if (!blank && indent <= def_indent)
            break;
         if (!blank)
            end = cur;
         while (*r && *r != '\n')
            r++;
      }
      return end;
   }

   /* C-family (and most brace languages): match the symbol's first `{` to its
    * closing `}`. No `{` (a declaration / one-liner) -> the start line. Braces
    * inside strings/comments can skew this; acceptable for an advisory span. */
   int depth = 0, seen_open = 0, cur = start_line, scanned = 0;
   for (const char *r = p; *r && scanned < 2000000; r++, scanned++)
   {
      char c = *r;
      if (c == '\n')
         cur++;
      else if (c == '{')
      {
         depth++;
         seen_open = 1;
      }
      else if (c == '}')
      {
         depth--;
         if (seen_open && depth <= 0)
            return cur;
      }
   }
   return start_line;
}

int extract_definitions(const char *ext, const char *content, definition_t *out, int max)
{
   /* §2: prefer the tree-sitter front-end where a grammar is compiled in; fall through
    * to the hand-rolled line scanners below when it's absent or has no grammar for
    * `ext` (the default build, with no -DAIMEE_TREESITTER, always falls through). */
   if (code_treesitter_available(ext))
   {
      int n = code_treesitter_definitions(ext, content, out, max);
      if (n >= 0)
      {
         /* Tree-sitter's C/C++ grammar does NOT emit preprocessor macros, but the
          * hand-rolled C path does (def_kind='macro'). Preserve them on the
          * tree-sitter path by appending a macro-only scan, so the extractor swap
          * is not a macro regression (cpp-class-method-extraction §3). Macro names
          * carry a distinct def_kind, so they never collide with the tree-sitter
          * function/type defs already in out[0..n). detect_lang maps every C/C++
          * extension (.c/.h/.cpp/.cc/.cxx/.hpp/.hh/.hxx/.inc) to LANG_C — there is no
          * separate LANG_CPP — so this gate covers all C++ headers/sources. (Edge: if
          * tree-sitter already filled out[] to `max`, the macro pass is skipped; the
          * defs array is sized generously by callers, so this is not hit in practice.) */
         int n_orig = n;
         if (n < max && detect_lang(ext) == LANG_C)
         {
            c_def_ctx_t mctx = {out, n, max, 0};
            for_each_line(content, c_macro_def_line, &mctx);
            n = mctx.count;
         }
         /* Fill body-end lines for ONLY the appended macro defs (tree-sitter already
          * set line_end for its own defs; never overwrite them). */
         for (int i = n_orig; i < n; i++)
            if (out[i].line >= 1 && out[i].line_end <= 0)
               out[i].line_end = code_def_end_line(content, out[i].line, ext);
         return n;
      }
   }
   lang_t lang = detect_lang(ext);
   def_ctx_t ctx = {out, 0, max, lang == LANG_TS};
   int count = 0;

   switch (lang)
   {
   case LANG_JS:
   case LANG_TS:
      for_each_line(content, js_def_line, &ctx);
      break;
   case LANG_PY:
      for_each_line(content, py_def_line, &ctx);
      break;
   case LANG_GO:
      for_each_line(content, go_def_line, &ctx);
      break;
   case LANG_CS:
      for_each_line(content, cs_def_line, &ctx);
      break;
   case LANG_SH:
      for_each_line(content, sh_def_line, &ctx);
      break;
   case LANG_DART:
      for_each_line(content, dart_def_line, &ctx);
      break;
   case LANG_C:
   {
      c_def_ctx_t cctx = {out, 0, max, 0};
      for_each_line(content, c_def_line, &cctx);
      count = cctx.count;
      break;
   }
   case LANG_LUA:
      for_each_line(content, lua_def_line, &ctx);
      break;
   case LANG_JAVA:
      for_each_line(content, java_def_line, &ctx);
      break;
   case LANG_RUST:
      for_each_line(content, rust_def_line, &ctx);
      break;
   case LANG_RUBY:
      for_each_line(content, ruby_def_line, &ctx);
      break;
   case LANG_KOTLIN:
      for_each_line(content, kotlin_def_line, &ctx);
      break;
   case LANG_SWIFT:
      for_each_line(content, swift_def_line, &ctx);
      break;
   case LANG_PHP:
      for_each_line(content, php_def_line, &ctx);
      break;
   default:
      break;
   }

   if (lang != LANG_C)
      count = ctx.count;
   /* Fill each definition's body end line from the source span. */
   for (int i = 0; i < count; i++)
      if (out[i].line >= 1)
         out[i].line_end = code_def_end_line(content, out[i].line, ext);
   return count;
}

int extract_calls(const char *ext, const char *content, call_ref_t *out, int max)
{
   /* Prefer the tree-sitter front-end where it is compiled in and handles this language;
    * fall back to the hand-rolled per-line extractors otherwise (and in the default
    * build, where code_treesitter_* is a stub). */
   if (code_treesitter_available(ext))
   {
      int n = code_treesitter_calls(ext, content, out, max);
      if (n >= 0)
         return n;
   }

   lang_t lang = detect_lang(ext);
   call_ctx_t ctx;
   memset(&ctx, 0, sizeof(ctx));
   ctx.out = out;
   ctx.max = max;

   switch (lang)
   {
   case LANG_C:
      for_each_line(content, c_call_line, &ctx);
      break;
   case LANG_PY:
      for_each_line(content, py_call_line, &ctx);
      break;
   case LANG_JS:
   case LANG_TS:
      for_each_line(content, js_call_line, &ctx);
      break;
   case LANG_GO:
      for_each_line(content, go_call_line, &ctx);
      break;
   case LANG_CS:
   case LANG_DART:
   case LANG_LUA:
   case LANG_JAVA:
   case LANG_RUST:
   case LANG_RUBY:
   case LANG_KOTLIN:
   case LANG_SWIFT:
   case LANG_PHP:
      for_each_line(content, generic_call_line, &ctx);
      break;
   default:
      break;
   }

   return ctx.count;
}
