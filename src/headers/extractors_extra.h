#ifndef DEC_EXTRACTORS_EXTRA_H
#define DEC_EXTRACTORS_EXTRA_H 1

#include "index.h"

/* Shared context types used by extractors */

typedef void (*line_fn)(const char *line, int lineno, void *ctx);

typedef struct
{
   char **out;
   int count;
   int max;
   int is_ts;
   /* H6: optional parallel flags array (NULL if not tracked). sys[i] = 1 when out[i]
    * is a system / angle-bracket include (C/C++ `#include <...>`); 0 otherwise. Used
    * so the route builder applies prefer-local only to quoted includes (angle
    * includes do not resolve to the caller's own directory). */
   int *sys;
} import_ctx_t;

typedef struct
{
   char **out;
   int count;
   int max;
} export_ctx_t;

typedef struct
{
   definition_t *out;
   int count;
   int max;
   int is_ts;
} def_ctx_t;

typedef struct
{
   definition_t *out;
   int count;
   int max;
   int in_block_comment;
} c_def_ctx_t;

typedef struct
{
   call_ref_t *out;
   int count;
   int max;
   char current_func[128]; /* tracks which function we're inside */
   int brace_depth;
   int in_block_comment;
} call_ctx_t;

/* Shared utility functions (defined in extractors.c) */
void for_each_line(const char *content, line_fn fn, void *ctx);
int add_str(char **out, int count, int max, const char *s);
int add_def(definition_t *out, int count, int max, const char *name, const char *kind, int line);
int add_call(call_ref_t *out, int count, int max, const char *caller, const char *callee, int line);
const char *extract_quoted(const char *p, char *buf, size_t len);
const char *skip_ws(const char *s);
const char *extract_ident(const char *p, char *buf, size_t len);

/* C# extractors */
void cs_import_line(const char *line, int lineno, void *ctx);
void cs_export_line(const char *line, int lineno, void *ctx);
void cs_route_line(const char *line, int lineno, void *ctx);
void cs_def_line(const char *line, int lineno, void *ctx);

/* Shell */
void sh_import_line(const char *line, int lineno, void *ctx);
void sh_def_line(const char *line, int lineno, void *ctx);

/* CSS */
void css_import_line(const char *line, int lineno, void *ctx);
void css_export_line(const char *line, int lineno, void *ctx);

/* Dart */
void dart_import_line(const char *line, int lineno, void *ctx);
void dart_export_line(const char *line, int lineno, void *ctx);
void dart_def_line(const char *line, int lineno, void *ctx);

/* C/C++ */
void c_import_line(const char *line, int lineno, void *ctx);
void c_export_line(const char *line, int lineno, void *ctx);
void c_def_line(const char *line, int lineno, void *ctx);

/* Lua */
void lua_import_line(const char *line, int lineno, void *ctx);
void lua_export_line(const char *line, int lineno, void *ctx);
void lua_def_line(const char *line, int lineno, void *ctx);

/* Java */
void java_import_line(const char *line, int lineno, void *ctx);
void java_export_line(const char *line, int lineno, void *ctx);
void java_route_line(const char *line, int lineno, void *ctx);
void java_def_line(const char *line, int lineno, void *ctx);

/* Rust */
void rust_import_line(const char *line, int lineno, void *ctx);
void rust_export_line(const char *line, int lineno, void *ctx);
void rust_def_line(const char *line, int lineno, void *ctx);

/* Ruby */
void ruby_import_line(const char *line, int lineno, void *ctx);
void ruby_export_line(const char *line, int lineno, void *ctx);
void ruby_route_line(const char *line, int lineno, void *ctx);
void ruby_def_line(const char *line, int lineno, void *ctx);

/* Kotlin */
void kotlin_import_line(const char *line, int lineno, void *ctx);
void kotlin_export_line(const char *line, int lineno, void *ctx);
void kotlin_route_line(const char *line, int lineno, void *ctx);
void kotlin_def_line(const char *line, int lineno, void *ctx);

/* Swift */
void swift_import_line(const char *line, int lineno, void *ctx);
void swift_export_line(const char *line, int lineno, void *ctx);
void swift_def_line(const char *line, int lineno, void *ctx);

/* PHP */
void php_import_line(const char *line, int lineno, void *ctx);
void php_export_line(const char *line, int lineno, void *ctx);
void php_def_line(const char *line, int lineno, void *ctx);

/* Call extraction (all languages) */
void c_call_line(const char *line, int lineno, void *ctx);
void py_call_line(const char *line, int lineno, void *ctx);
void js_call_line(const char *line, int lineno, void *ctx);
void go_call_line(const char *line, int lineno, void *ctx);
void generic_call_line(const char *line, int lineno, void *ctx);

#endif
