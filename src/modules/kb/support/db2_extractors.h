#ifndef AIMEE_DB2_EXTRACTORS_H
#define AIMEE_DB2_EXTRACTORS_H 1

#include <stddef.h>

/* Tests compile this generated unit beside the authoritative monolith. Prefix
 * every global so the linker can compare both implementations in one process. */
#ifdef AIMEE_DB2_EXTRACTOR_PREFIX
#define add_call            db2_support_add_call
#define add_def             db2_support_add_def
#define add_str             db2_support_add_str
#define c_call_line         db2_support_c_call_line
#define c_def_line          db2_support_c_def_line
#define c_export_line       db2_support_c_export_line
#define c_import_line       db2_support_c_import_line
#define c_macro_def_line    db2_support_c_macro_def_line
#define code_def_end_line   db2_support_code_def_end_line
#define cs_def_line         db2_support_cs_def_line
#define cs_export_line      db2_support_cs_export_line
#define cs_import_line      db2_support_cs_import_line
#define cs_route_line       db2_support_cs_route_line
#define css_export_line     db2_support_css_export_line
#define css_import_line     db2_support_css_import_line
#define dart_def_line       db2_support_dart_def_line
#define dart_export_line    db2_support_dart_export_line
#define dart_import_line    db2_support_dart_import_line
#define extract_calls       db2_support_extract_calls
#define extract_definitions db2_support_extract_definitions
#define extract_exports     db2_support_extract_exports
#define extract_ident       db2_support_extract_ident
#define extract_imports     db2_support_extract_imports
#define extract_imports_sys db2_support_extract_imports_sys
#define extract_quoted      db2_support_extract_quoted
#define extract_routes      db2_support_extract_routes
#define for_each_line       db2_support_for_each_line
#define generic_call_line   db2_support_generic_call_line
#define go_call_line        db2_support_go_call_line
#define index_has_extractor db2_support_index_has_extractor
#define java_def_line       db2_support_java_def_line
#define java_export_line    db2_support_java_export_line
#define java_import_line    db2_support_java_import_line
#define java_route_line     db2_support_java_route_line
#define js_call_line        db2_support_js_call_line
#define kotlin_def_line     db2_support_kotlin_def_line
#define kotlin_export_line  db2_support_kotlin_export_line
#define kotlin_import_line  db2_support_kotlin_import_line
#define kotlin_route_line   db2_support_kotlin_route_line
#define lua_def_line        db2_support_lua_def_line
#define lua_export_line     db2_support_lua_export_line
#define lua_import_line     db2_support_lua_import_line
#define php_def_line        db2_support_php_def_line
#define php_export_line     db2_support_php_export_line
#define php_import_line     db2_support_php_import_line
#define py_call_line        db2_support_py_call_line
#define ruby_def_line       db2_support_ruby_def_line
#define ruby_export_line    db2_support_ruby_export_line
#define ruby_import_line    db2_support_ruby_import_line
#define ruby_route_line     db2_support_ruby_route_line
#define rust_def_line       db2_support_rust_def_line
#define rust_export_line    db2_support_rust_export_line
#define rust_import_line    db2_support_rust_import_line
#define sh_def_line         db2_support_sh_def_line
#define sh_import_line      db2_support_sh_import_line
#define skip_ws             db2_support_skip_ws
#define swift_def_line      db2_support_swift_def_line
#define swift_export_line   db2_support_swift_export_line
#define swift_import_line   db2_support_swift_import_line
#endif

typedef struct
{
   char name[128];
   char kind[32];
   int line;
   int line_end;
} db2_definition_t;

typedef struct
{
   char caller[128];
   char callee[128];
   int line;
} db2_call_ref_t;

typedef void (*line_fn)(const char *line, int lineno, void *ctx);

typedef struct
{
   char **out;
   int count;
   int max;
   int is_ts;
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
   db2_definition_t *out;
   int count;
   int max;
   int is_ts;
} def_ctx_t;

typedef struct
{
   db2_definition_t *out;
   int count;
   int max;
   int in_block_comment;
} c_def_ctx_t;

typedef struct
{
   db2_call_ref_t *out;
   int count;
   int max;
   char current_func[128];
   int brace_depth;
   int in_block_comment;
} call_ctx_t;

void for_each_line(const char *content, line_fn fn, void *ctx);
int add_str(char **out, int count, int max, const char *s);
int add_def(db2_definition_t *out, int count, int max, const char *name, const char *kind,
            int line);
int add_call(db2_call_ref_t *out, int count, int max, const char *caller, const char *callee,
             int line);
const char *extract_quoted(const char *p, char *buf, size_t len);
const char *skip_ws(const char *s);
const char *extract_ident(const char *p, char *buf, size_t len);

void cs_import_line(const char *, int, void *);
void cs_export_line(const char *, int, void *);
void cs_route_line(const char *, int, void *);
void cs_def_line(const char *, int, void *);
void sh_import_line(const char *, int, void *);
void sh_def_line(const char *, int, void *);
void css_import_line(const char *, int, void *);
void css_export_line(const char *, int, void *);
void dart_import_line(const char *, int, void *);
void dart_export_line(const char *, int, void *);
void dart_def_line(const char *, int, void *);
void c_import_line(const char *, int, void *);
void c_export_line(const char *, int, void *);
void c_def_line(const char *, int, void *);
void c_macro_def_line(const char *, int, void *);
void lua_import_line(const char *, int, void *);
void lua_export_line(const char *, int, void *);
void lua_def_line(const char *, int, void *);
void java_import_line(const char *, int, void *);
void java_export_line(const char *, int, void *);
void java_route_line(const char *, int, void *);
void java_def_line(const char *, int, void *);
void rust_import_line(const char *, int, void *);
void rust_export_line(const char *, int, void *);
void rust_def_line(const char *, int, void *);
void ruby_import_line(const char *, int, void *);
void ruby_export_line(const char *, int, void *);
void ruby_route_line(const char *, int, void *);
void ruby_def_line(const char *, int, void *);
void kotlin_import_line(const char *, int, void *);
void kotlin_export_line(const char *, int, void *);
void kotlin_route_line(const char *, int, void *);
void kotlin_def_line(const char *, int, void *);
void swift_import_line(const char *, int, void *);
void swift_export_line(const char *, int, void *);
void swift_def_line(const char *, int, void *);
void php_import_line(const char *, int, void *);
void php_export_line(const char *, int, void *);
void php_def_line(const char *, int, void *);
void c_call_line(const char *, int, void *);
void py_call_line(const char *, int, void *);
void js_call_line(const char *, int, void *);
void go_call_line(const char *, int, void *);
void generic_call_line(const char *, int, void *);

#ifndef AIMEE_DB2_EXTRACTOR_NO_PUBLIC_PROTOTYPES
int extract_imports_sys(const char *ext, const char *content, char **out, int *sys, int max);
int extract_exports(const char *ext, const char *content, char **out, int max);
int extract_routes(const char *ext, const char *content, char **out, int max);
int extract_definitions(const char *ext, const char *content, db2_definition_t *out, int max);
int extract_calls(const char *ext, const char *content, db2_call_ref_t *out, int max);
#endif

#endif /* AIMEE_DB2_EXTRACTORS_H */
