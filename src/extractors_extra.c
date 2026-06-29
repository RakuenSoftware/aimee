/* extractors_extra.c: language extractors for C#, Shell, CSS, Dart, C/C++, Lua */
#include "aimee.h"
#include "extractors_extra.h"
#include "c_system_headers.h"
#include <ctype.h>

/* --- C# --- */

void cs_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   if (strncmp(p, "using ", 6) == 0)
   {
      p += 6;
      p = skip_ws(p);
      /* Read until ; */
      char buf[512];
      size_t i = 0;
      while (p[i] && p[i] != ';' && i < sizeof(buf) - 1)
      {
         buf[i] = p[i];
         i++;
      }
      buf[i] = '\0';
      if (i > 0)
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
   }
}

void cs_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;

   (void)lineno;

   if (!strstr(line, "public "))
      return;

   static const char *kw[] = {"class ", "interface ", "enum ", "struct ", "record ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      const char *p = strstr(line, kw[i]);
      if (p)
      {
         p += strlen(kw[i]);
         char name[256];
         if (extract_ident(p, name, sizeof(name)))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }
}

void cs_route_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *rc = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   static const char *attrs[] = {"[HttpGet(",    "[HttpPost(", "[HttpPut(",
                                 "[HttpDelete(", "[Route(",    NULL};

   /* Attribute with path */
   for (int i = 0; attrs[i]; i++)
   {
      if (strncmp(p, attrs[i], strlen(attrs[i])) == 0)
      {
         const char *q = p + strlen(attrs[i]);
         char buf[512];
         if (extract_quoted(q, buf, sizeof(buf)))
            rc->count = add_str(rc->out, rc->count, rc->max, buf);
         return;
      }
   }

   /* Bare attributes without path */
   static const char *bare[] = {"[HttpGet]", "[HttpPost]", "[HttpPut]", "[HttpDelete]", NULL};
   for (int i = 0; bare[i]; i++)
   {
      if (strstr(p, bare[i]))
      {
         rc->count = add_str(rc->out, rc->count, rc->max, bare[i]);
         return;
      }
   }
}

void cs_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;

   static const char *kw[] = {"class ", "interface ", "enum ", "struct ", "record ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      const char *p = strstr(line, kw[i]);
      if (p)
      {
         p += strlen(kw[i]);
         char name[256];
         if (extract_ident(p, name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
   }

   /* Method declarations (public/private/etc. + return type + name) */
   const char *p = skip_ws(line);
   static const char *access[] = {"public ", "private ", "protected ", "internal ", NULL};
   for (int i = 0; access[i]; i++)
   {
      if (strncmp(p, access[i], strlen(access[i])) == 0)
      {
         /* Skip modifiers: static, async, virtual, override, etc. */
         p += strlen(access[i]);
         while (strncmp(p, "static ", 7) == 0 || strncmp(p, "async ", 6) == 0 ||
                strncmp(p, "virtual ", 8) == 0 || strncmp(p, "override ", 9) == 0 ||
                strncmp(p, "abstract ", 9) == 0)
         {
            const char *sp = strchr(p, ' ');
            if (!sp)
               return;
            p = sp + 1;
         }
         /* Skip return type */
         char dummy[256];
         if (!extract_ident(p, dummy, sizeof(dummy)))
            return;
         p += strlen(dummy);
         /* Check for generic <T> */
         if (*p == '<')
         {
            p = strchr(p, '>');
            if (!p)
               return;
            p++;
         }
         p = skip_ws(p);
         /* Next identifier is the method name if followed by ( */
         char name[256];
         if (extract_ident(p, name, sizeof(name)))
         {
            p += strlen(name);
            if (*p == '(')
               dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         }
         return;
      }
   }
}

/* --- Shell --- */

void sh_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   if (strncmp(p, "source ", 7) == 0)
   {
      p += 7;
      p = skip_ws(p);
      char buf[512];
      size_t i = 0;
      while (p[i] && !isspace((unsigned char)p[i]) && i < sizeof(buf) - 1)
      {
         buf[i] = p[i];
         i++;
      }
      buf[i] = '\0';
      if (i > 0)
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
      return;
   }

   /* . /path/to/file (dot source, must be followed by space) */
   if (p[0] == '.' && p[1] == ' ')
   {
      p += 2;
      p = skip_ws(p);
      char buf[512];
      size_t i = 0;
      while (p[i] && !isspace((unsigned char)p[i]) && i < sizeof(buf) - 1)
      {
         buf[i] = p[i];
         i++;
      }
      buf[i] = '\0';
      if (i > 0)
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
   }
}

void sh_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   /* function name or name() */
   if (strncmp(p, "function ", 9) == 0)
   {
      char name[256];
      if (extract_ident(p + 9, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }

   /* name() { */
   char name[256];
   if (extract_ident(p, name, sizeof(name)))
   {
      const char *after = p + strlen(name);
      if (after[0] == '(' && after[1] == ')')
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
   }
}

/* --- CSS --- */

void css_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   if (strncmp(p, "@import ", 8) == 0)
   {
      p += 8;
      p = skip_ws(p);
      char buf[512];
      if (extract_quoted(p, buf, sizeof(buf)))
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
   }
}

void css_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* .class-name { */
   if (*p == '.')
   {
      p++;
      char name[256];
      size_t i = 0;
      while (i < sizeof(name) - 1 && (isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '_'))
      {
         name[i] = p[i];
         i++;
      }
      name[i] = '\0';
      if (i > 0)
         ec->count = add_str(ec->out, ec->count, ec->max, name);
      return;
   }

   /* --custom-property */
   if (p[0] == '-' && p[1] == '-')
   {
      char name[256];
      size_t i = 0;
      while (i < sizeof(name) - 1 && (isalnum((unsigned char)p[i]) || p[i] == '-' || p[i] == '_'))
      {
         name[i] = p[i];
         i++;
      }
      name[i] = '\0';
      if (i > 0)
         ec->count = add_str(ec->out, ec->count, ec->max, name);
   }
}

/* --- Dart --- */

void dart_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   if (strncmp(p, "import ", 7) == 0)
   {
      p += 7;
      p = skip_ws(p);
      char buf[512];
      if (extract_quoted(p, buf, sizeof(buf)))
         ic->count = add_str(ic->out, ic->count, ic->max, buf);
   }
}

void dart_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   static const char *kw[] = {"class ", "mixin ", "enum ", "extension ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      if (strncmp(p, kw[i], strlen(kw[i])) == 0)
      {
         char name[256];
         if (extract_ident(p + strlen(kw[i]), name, sizeof(name)))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }
}

void dart_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   static const char *kw[] = {"class ", "mixin ", "enum ", NULL};

   for (int i = 0; kw[i]; i++)
   {
      if (strncmp(p, kw[i], strlen(kw[i])) == 0)
      {
         char name[256];
         if (extract_ident(p + strlen(kw[i]), name, sizeof(name)))
            dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         return;
      }
   }

   /* Top-level function: return_type name( at indent 0 */
   if (line[0] != ' ' && line[0] != '\t' && line[0] != '/')
   {
      /* Look for identifier followed by identifier followed by ( */
      char ret[256], name[256];
      if (extract_ident(p, ret, sizeof(ret)))
      {
         const char *np = p + strlen(ret);
         np = skip_ws(np);
         if (extract_ident(np, name, sizeof(name)))
         {
            np += strlen(name);
            if (*np == '(')
               dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
         }
      }
   }
}

/* --- C/C++ --- */

void c_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* Capture BOTH #include "local.h" (is_system=0) and #include <lib.h>
    * (is_system=1). H6: angle includes are no longer dropped — a `<Foo.h>` is how
    * an INSTALLED external lib is included, so dropping it lost real cross-repo
    * deps (e.g. moonlight-qt -> moonlight-common-c via <Limelight.h>). The route
    * builder keys on is_system to skip prefer-local for angle includes (angle does
    * not resolve to the caller's own dir); system headers that match no repo file
    * still produce no route. */
   if (strncmp(p, "#include", 8) != 0)
      return;
   p += 8;
   p = skip_ws(p);

   char buf[512];
   int is_sys = 0;
   if (*p == '"')
   {
      if (!extract_quoted(p, buf, sizeof(buf)))
         return;
   }
   else if (*p == '<')
   {
      const char *e = strchr(p + 1, '>');
      if (!e)
         return;
      size_t len = (size_t)(e - (p + 1));
      if (len == 0 || len >= sizeof(buf))
         return;
      memcpy(buf, p + 1, len);
      buf[len] = '\0';
      is_sys = 1;
      /* Drop C/C++ stdlib/system angle includes up front (never cross-repo). */
      if (aimee_c_system_header_is(buf))
         return;
   }
   else
      return;

   int before = ic->count;
   ic->count = add_str(ic->out, ic->count, ic->max, buf);
   if (ic->sys && ic->count > before)
      ic->sys[before] = is_sys;
}

void c_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* Skip static functions (not exported) */
   if (strncmp(p, "static ", 7) == 0)
      return;

   /* Skip preprocessor lines */
   if (*p == '#')
      return;

   /* Type declarations: struct/enum/union/typedef with a name */
   static const char *type_kw[] = {"struct ", "enum ", "union ", NULL};
   for (int i = 0; type_kw[i]; i++)
   {
      if (strncmp(p, type_kw[i], strlen(type_kw[i])) == 0)
      {
         char name[256];
         if (extract_ident(p + strlen(type_kw[i]), name, sizeof(name)))
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         return;
      }
   }

   /* typedef: extract the last identifier before ; */
   if (strncmp(p, "typedef ", 8) == 0)
   {
      const char *semi = strrchr(p, ';');
      if (semi)
      {
         /* Walk backwards from semicolon to find the typedef name */
         const char *end = semi - 1;
         while (end > p && isspace((unsigned char)*end))
            end--;
         const char *start = end;
         while (start > p && (isalnum((unsigned char)*(start - 1)) || *(start - 1) == '_'))
            start--;
         size_t nlen = (size_t)(end - start + 1);
         if (nlen > 0 && nlen < 256)
         {
            char name[256];
            memcpy(name, start, nlen);
            name[nlen] = '\0';
            ec->count = add_str(ec->out, ec->count, ec->max, name);
         }
      }
      return;
   }

   /* Function declarations: type name( at indent 0 */
   if (line[0] == ' ' || line[0] == '\t')
      return;
   char ret[256];
   if (!extract_ident(p, ret, sizeof(ret)))
      return;
   const char *np = p + strlen(ret);
   /* Skip pointer stars and spaces */
   while (*np == ' ' || *np == '*')
      np++;
   char name[256];
   if (extract_ident(np, name, sizeof(name)))
   {
      np += strlen(name);
      if (*np == '(')
         ec->count = add_str(ec->out, ec->count, ec->max, name);
   }
}

/* Recognise env-var-style string-literal args to getenv/setenv/putenv/
 * unsetenv. Operators (and CLAUDE.md) treat `aimee index find <SYM>` as
 * a grep replacement; without this, env-var names like AIMEE_PROFILE
 * never show up because they live in string literals, not in any
 * declaration we recognise. Indexed as kind="env_var" so the result
 * line distinguishes them from real C symbols. */
static void c_scan_env_vars(const char *line, int lineno, c_def_ctx_t *dc)
{
   static const char *const patterns[] = {
       "getenv(", "setenv(", "putenv(", "unsetenv(", NULL,
   };
   for (int i = 0; patterns[i]; i++)
   {
      size_t plen = strlen(patterns[i]);
      const char *p = line;
      while ((p = strstr(p, patterns[i])) != NULL)
      {
         /* Reject prefix matches like aimee_getenv(...) so we don't
          * pick up wrappers as if they were the stdlib calls. */
         int has_word_prefix = 0;
         if (p > line)
         {
            char prev = p[-1];
            if (isalnum((unsigned char)prev) || prev == '_')
               has_word_prefix = 1;
         }
         p += plen;
         if (has_word_prefix)
            continue;
         while (*p == ' ' || *p == '\t')
            p++;
         if (*p == '"')
         {
            char buf[256];
            if (extract_quoted(p, buf, sizeof(buf)) && buf[0])
               dc->count = add_def(dc->out, dc->count, dc->max, buf, "env_var", lineno);
         }
      }
   }
}

/* Macro-only scan (see header): preserve `#define NAME` macros when tree-sitter
 * handles a C/C++ file. Comment-aware so a #define inside a block/line comment is
 * not captured — mirrors c_def_line's comment handling exactly. */
void c_macro_def_line(const char *line, int lineno, void *ctx)
{
   c_def_ctx_t *dc = (c_def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   if (dc->in_block_comment)
   {
      if (strstr(line, "*/"))
         dc->in_block_comment = 0;
      return;
   }
   if (strstr(p, "/*") && !strstr(p, "*/"))
   {
      dc->in_block_comment = 1;
      return;
   }
   if (p[0] == '/' && p[1] == '/')
      return;

   if (strncmp(p, "#define ", 8) == 0)
   {
      char name[256];
      if (extract_ident(p + 8, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "macro", lineno);
   }
}

void c_def_line(const char *line, int lineno, void *ctx)
{
   c_def_ctx_t *dc = (c_def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   /* Track block comments */
   if (dc->in_block_comment)
   {
      if (strstr(line, "*/"))
         dc->in_block_comment = 0;
      return;
   }
   if (strstr(p, "/*") && !strstr(p, "*/"))
   {
      dc->in_block_comment = 1;
      return;
   }

   /* Skip single-line comments */
   if (p[0] == '/' && p[1] == '/')
      return;

   /* env-var args run on every code line — these typically live inside
    * function bodies (indented), so we scan before the indent guard
    * below that returns early for non-zero-column lines. */
   c_scan_env_vars(line, lineno, dc);

   /* #define NAME */
   if (strncmp(p, "#define ", 8) == 0)
   {
      char name[256];
      if (extract_ident(p + 8, name, sizeof(name)))
         /* H0a: macro (SDK-prone, e.g. DEFINE_GUID) — ineligible HIGH definer (§5). */
         dc->count = add_def(dc->out, dc->count, dc->max, name, "macro", lineno);
      return;
   }

   /* Skip other preprocessor lines */
   if (*p == '#')
      return;

   /* struct/enum/union name (H0a: keep the keyword as the granular kind). */
   static const char *type_kw[] = {"struct ", "enum ", "union ", NULL};
   static const char *type_kind[] = {"struct", "enum", "union", NULL};
   for (int i = 0; type_kw[i]; i++)
   {
      const char *kp = strstr(p, type_kw[i]);
      if (kp)
      {
         const char *np = kp + strlen(type_kw[i]);
         char name[256];
         if (extract_ident(np, name, sizeof(name)) && name[0] != '{')
            dc->count = add_def(dc->out, dc->count, dc->max, name, type_kind[i], lineno);
         return;
      }
   }

   /* typedef: extract the last identifier before ; */
   if (strncmp(p, "typedef ", 8) == 0)
   {
      const char *semi = strrchr(p, ';');
      if (semi)
      {
         const char *end = semi - 1;
         while (end > p && isspace((unsigned char)*end))
            end--;
         const char *start = end;
         while (start > p && (isalnum((unsigned char)*(start - 1)) || *(start - 1) == '_'))
            start--;
         size_t nlen = (size_t)(end - start + 1);
         if (nlen > 0 && nlen < 256)
         {
            char name[256];
            memcpy(name, start, nlen);
            name[nlen] = '\0';
            /* H0a: typedef (SDK-prone, e.g. EGLDisplay) — ineligible HIGH definer (§5). */
            dc->count = add_def(dc->out, dc->count, dc->max, name, "typedef", lineno);
         }
      }
      return;
   }

   /* Function definition: must be at indent 0 */
   if (line[0] == ' ' || line[0] == '\t')
      return;
   /* Skip if line contains only a closing brace or is empty */
   if (*p == '{' || *p == '}' || *p == '\0')
      return;

   /* Skip 'static' keyword but still index as definition */
   const char *fp = p;
   if (strncmp(fp, "static ", 7) == 0)
      fp += 7;
   /* Skip qualifiers */
   while (strncmp(fp, "inline ", 7) == 0 || strncmp(fp, "const ", 6) == 0 ||
          strncmp(fp, "extern ", 7) == 0)
   {
      const char *sp = strchr(fp, ' ');
      if (!sp)
         return;
      fp = sp + 1;
   }

   /* type name( pattern */
   char ret[256];
   if (!extract_ident(fp, ret, sizeof(ret)))
      return;
   const char *np = fp + strlen(ret);
   /* Skip pointer stars and spaces */
   while (*np == ' ' || *np == '*')
      np++;
   char name[256];
   if (extract_ident(np, name, sizeof(name)))
   {
      np += strlen(name);
      if (*np == '(')
         /* H0a: function — a real first-party API surface, eligible HIGH definer (§5). */
         dc->count = add_def(dc->out, dc->count, dc->max, name, "function", lineno);
   }
}

/* --- Lua --- */

void lua_import_line(const char *line, int lineno, void *ctx)
{
   import_ctx_t *ic = (import_ctx_t *)ctx;

   (void)lineno;

   /* require("name") or require 'name' or require "name" */
   const char *p = strstr(line, "require");
   if (!p)
      return;
   p += 7;
   p = skip_ws(p);

   /* Skip optional ( */
   if (*p == '(')
      p++;
   p = skip_ws(p);

   char buf[512];
   if (extract_quoted(p, buf, sizeof(buf)))
      ic->count = add_str(ic->out, ic->count, ic->max, buf);
}

void lua_export_line(const char *line, int lineno, void *ctx)
{
   export_ctx_t *ec = (export_ctx_t *)ctx;
   const char *p = skip_ws(line);

   (void)lineno;

   /* function M.name( or function M:name( */
   if (strncmp(p, "function ", 9) == 0)
   {
      const char *np = p + 9;
      np = skip_ws(np);
      char full[256];
      size_t i = 0;
      while (i < sizeof(full) - 1 &&
             (isalnum((unsigned char)np[i]) || np[i] == '_' || np[i] == '.' || np[i] == ':'))
      {
         full[i] = np[i];
         i++;
      }
      full[i] = '\0';
      /* Only export if it has a dot or colon (module method) */
      if (strchr(full, '.') || strchr(full, ':'))
      {
         /* Extract just the method name after . or : */
         const char *method = strrchr(full, '.');
         if (!method)
            method = strrchr(full, ':');
         if (method)
            ec->count = add_str(ec->out, ec->count, ec->max, method + 1);
      }
   }
}

void lua_def_line(const char *line, int lineno, void *ctx)
{
   def_ctx_t *dc = (def_ctx_t *)ctx;
   const char *p = skip_ws(line);

   /* local function name( */
   if (strncmp(p, "local function ", 15) == 0)
   {
      char name[256];
      if (extract_ident(p + 15, name, sizeof(name)))
         dc->count = add_def(dc->out, dc->count, dc->max, name, "definition", lineno);
      return;
   }

   /* function name( or function M.name( or function M:name( */
   if (strncmp(p, "function ", 9) == 0)
   {
      const char *np = p + 9;
      np = skip_ws(np);
      char full[256];
      size_t i = 0;
      while (i < sizeof(full) - 1 &&
             (isalnum((unsigned char)np[i]) || np[i] == '_' || np[i] == '.' || np[i] == ':'))
      {
         full[i] = np[i];
         i++;
      }
      full[i] = '\0';
      if (full[0])
         dc->count = add_def(dc->out, dc->count, dc->max, full, "definition", lineno);
      return;
   }
}

/* --- Call extraction: scan lines for function call patterns --- */

/* Helper: check if a string is a C keyword (not a function call) */
static int is_c_keyword(const char *s)
{
   static const char *kw[] = {"if",       "else",   "while",   "for",     "switch", "case",
                              "return",   "sizeof", "typeof",  "alignof", "do",     "break",
                              "continue", "goto",   "default", NULL};
   for (int i = 0; kw[i]; i++)
      if (strcmp(s, kw[i]) == 0)
         return 1;
   return 0;
}

/* Helper: check if a string is a Python keyword or builtin */
static int is_py_keyword(const char *s)
{
   static const char *kw[] = {
       "if",     "elif",   "else",     "for",      "while", "def",   "class",  "return",  "import",
       "from",   "with",   "as",       "yield",    "raise", "try",   "except", "finally", "assert",
       "del",    "pass",   "break",    "continue", "and",   "or",    "not",    "in",      "is",
       "lambda", "global", "nonlocal", "async",    "await", "print", NULL};
   for (int i = 0; kw[i]; i++)
      if (strcmp(s, kw[i]) == 0)
         return 1;
   return 0;
}

/* Helper: check if a string is a JS keyword */
static int is_js_keyword(const char *s)
{
   static const char *kw[] = {"if",     "else",     "while",      "for",   "switch", "case",
                              "return", "typeof",   "instanceof", "new",   "delete", "throw",
                              "catch",  "finally",  "do",         "void",  "in",     "of",
                              "class",  "import",   "export",     "from",  "const",  "let",
                              "var",    "function", "async",      "await", "yield",  NULL};
   for (int i = 0; kw[i]; i++)
      if (strcmp(s, kw[i]) == 0)
         return 1;
   return 0;
}

/* Scan a line for identifier( patterns, extracting function calls.
 * Works for C-family syntax. */
static void scan_calls_in_line(const char *line, int lineno, call_ctx_t *cc,
                               int (*is_kw)(const char *))
{
   const char *p = line;
   while (*p)
   {
      /* Skip string literals */
      if (*p == '"' || *p == '\'')
      {
         char q = *p++;
         while (*p && *p != q)
         {
            if (*p == '\\' && p[1])
               p++;
            p++;
         }
         if (*p)
            p++;
         continue;
      }

      /* Skip line comments */
      if (p[0] == '/' && p[1] == '/')
         return;

      /* Look for identifier( pattern */
      if (isalpha((unsigned char)*p) || *p == '_')
      {
         const char *start = p;
         while (isalnum((unsigned char)*p) || *p == '_')
            p++;
         size_t ilen = (size_t)(p - start);

         /* Skip whitespace between identifier and ( */
         const char *after = p;
         while (*after == ' ' || *after == '\t')
            after++;

         if (*after == '(' && ilen > 0 && ilen < 128)
         {
            char name[128];
            memcpy(name, start, ilen);
            name[ilen] = '\0';

            if (!is_kw(name))
               cc->count = add_call(cc->out, cc->count, cc->max, cc->current_func, name, lineno);
         }
         continue;
      }

      p++;
   }
}

/* C call extractor: tracks current function via brace depth */
void c_call_line(const char *line, int lineno, void *ctx)
{
   call_ctx_t *cc = (call_ctx_t *)ctx;
   const char *p = skip_ws(line);

   /* Track block comments */
   if (cc->in_block_comment)
   {
      if (strstr(line, "*/"))
         cc->in_block_comment = 0;
      return;
   }
   if (strstr(p, "/*") && !strstr(p, "*/"))
   {
      cc->in_block_comment = 1;
      return;
   }
   if (p[0] == '/' && p[1] == '/')
      return;
   if (*p == '#')
      return;

   /* Track brace depth for current function context */
   for (const char *b = line; *b; b++)
   {
      if (*b == '"' || *b == '\'')
      {
         char q = *b++;
         while (*b && *b != q)
         {
            if (*b == '\\' && b[1])
               b++;
            b++;
         }
         if (!*b)
            break;
         continue;
      }
      if (*b == '{')
         cc->brace_depth++;
      else if (*b == '}')
      {
         cc->brace_depth--;
         if (cc->brace_depth <= 0)
         {
            cc->current_func[0] = '\0';
            cc->brace_depth = 0;
         }
      }
   }

   /* Detect function definition: type name( at column 0 */
   if (cc->brace_depth == 0 && line[0] != ' ' && line[0] != '\t' && *p != '{' && *p != '}')
   {
      const char *fp = p;
      if (strncmp(fp, "static ", 7) == 0)
         fp += 7;
      while (strncmp(fp, "inline ", 7) == 0 || strncmp(fp, "const ", 6) == 0 ||
             strncmp(fp, "extern ", 7) == 0)
      {
         const char *sp = strchr(fp, ' ');
         if (!sp)
            break;
         fp = sp + 1;
      }
      char ret[128], fname[128];
      if (extract_ident(fp, ret, sizeof(ret)))
      {
         const char *np = fp + strlen(ret);
         while (*np == ' ' || *np == '*')
            np++;
         if (extract_ident(np, fname, sizeof(fname)))
         {
            np += strlen(fname);
            if (*np == '(')
               snprintf(cc->current_func, sizeof(cc->current_func), "%s", fname);
         }
      }
   }

   /* Extract calls from this line */
   scan_calls_in_line(line, lineno, cc, is_c_keyword);
}

/* Python call extractor: tracks current function via def lines */
void py_call_line(const char *line, int lineno, void *ctx)
{
   call_ctx_t *cc = (call_ctx_t *)ctx;
   const char *p = skip_ws(line);

   if (*p == '#' || *p == '\0')
      return;

   /* Track current function: def name( */
   if (strncmp(p, "def ", 4) == 0)
   {
      char name[128];
      if (extract_ident(p + 4, name, sizeof(name)))
         snprintf(cc->current_func, sizeof(cc->current_func), "%s", name);
   }

   scan_calls_in_line(line, lineno, cc, is_py_keyword);
}

/* JS/TS call extractor */
void js_call_line(const char *line, int lineno, void *ctx)
{
   call_ctx_t *cc = (call_ctx_t *)ctx;
   const char *p = skip_ws(line);

   if ((p[0] == '/' && p[1] == '/') || *p == '\0')
      return;

   /* Track current function: function name( or name( { or const name = */
   if (strncmp(p, "function ", 9) == 0)
   {
      char name[128];
      if (extract_ident(p + 9, name, sizeof(name)))
         snprintf(cc->current_func, sizeof(cc->current_func), "%s", name);
   }

   /* Track brace depth */
   for (const char *b = line; *b; b++)
   {
      if (*b == '"' || *b == '\'' || *b == '`')
      {
         char q = *b++;
         while (*b && *b != q)
         {
            if (*b == '\\' && b[1])
               b++;
            b++;
         }
         if (!*b)
            break;
         continue;
      }
      if (*b == '{')
         cc->brace_depth++;
      else if (*b == '}')
      {
         cc->brace_depth--;
         if (cc->brace_depth <= 0)
         {
            cc->current_func[0] = '\0';
            cc->brace_depth = 0;
         }
      }
   }

   scan_calls_in_line(line, lineno, cc, is_js_keyword);
}

/* Go call extractor */
void go_call_line(const char *line, int lineno, void *ctx)
{
   call_ctx_t *cc = (call_ctx_t *)ctx;
   const char *p = skip_ws(line);

   if ((p[0] == '/' && p[1] == '/') || *p == '\0')
      return;

   /* Track current function: func name( */
   if (strncmp(p, "func ", 5) == 0)
   {
      const char *np = p + 5;
      /* Skip receiver: func (r *Recv) Name( */
      if (*np == '(')
      {
         np = strchr(np, ')');
         if (np)
            np = skip_ws(np + 1);
         else
            return;
      }
      char name[128];
      if (extract_ident(np, name, sizeof(name)))
         snprintf(cc->current_func, sizeof(cc->current_func), "%s", name);
   }

   /* Track brace depth */
   for (const char *b = line; *b; b++)
   {
      if (*b == '"' || *b == '\'' || *b == '`')
      {
         char q = *b++;
         while (*b && *b != q)
         {
            if (*b == '\\' && b[1])
               b++;
            b++;
         }
         if (!*b)
            break;
         continue;
      }
      if (*b == '{')
         cc->brace_depth++;
      else if (*b == '}')
      {
         cc->brace_depth--;
         if (cc->brace_depth <= 0)
         {
            cc->current_func[0] = '\0';
            cc->brace_depth = 0;
         }
      }
   }

   scan_calls_in_line(line, lineno, cc, is_c_keyword);
}

/* Generic call extractor for other languages */
void generic_call_line(const char *line, int lineno, void *ctx)
{
   call_ctx_t *cc = (call_ctx_t *)ctx;
   scan_calls_in_line(line, lineno, cc, is_c_keyword);
}
