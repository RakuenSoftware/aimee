#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "extractors_extra.h"

/* --- C extractor tests --- */

static void test_c_imports(void)
{
   char *imports[16];
   int sys[16];
   memset(imports, 0, sizeof(imports));
   memset(sys, 0, sizeof(sys));
   import_ctx_t ic = {imports, 0, 16, 0, sys};

   c_import_line("#include \"foo.h\"", 1, &ic);
   assert(ic.count == 1);
   assert(strcmp(imports[0], "foo.h") == 0);
   assert(sys[0] == 0); /* quoted */

   c_import_line("#include \"bar/baz.h\"", 2, &ic);
   assert(ic.count == 2);
   assert(strcmp(imports[1], "bar/baz.h") == 0);

   /* H6: a real external-lib angle include <lib.h> IS captured with is_system=1
    * (so it can form a cross-repo route; the builder uses is_system to skip
    * prefer-local for it). Path is preserved verbatim for path-qualified includes. */
   c_import_line("#include <Limelight.h>", 3, &ic);
   assert(ic.count == 3);
   assert(strcmp(imports[2], "Limelight.h") == 0);
   assert(sys[2] == 1); /* angle */

   c_import_line("#include <libavutil/hwcontext.h>", 4, &ic);
   assert(ic.count == 4);
   assert(strcmp(imports[3], "libavutil/hwcontext.h") == 0); /* full path, NOT bare */
   assert(sys[3] == 1);

   /* BARE C/C++ stdlib/system angle includes are dropped at extraction (never
    * cross-repo) so they don't bloat file_imports or exhaust the import buffer. */
   c_import_line("#include <stdio.h>", 5, &ic);
   assert(ic.count == 4);
   c_import_line("#include <vector>", 6, &ic);
   assert(ic.count == 4);
   /* H7: Windows system headers (angle, _WIN32 paths) are dropped too — closes the
    * <process.h>-vs-a-repo's-process.h incidental-collision FP found in H4. */
   c_import_line("#include <process.h>", 7, &ic);
   assert(ic.count == 4);
   c_import_line("#include <windows.h>", 8, &ic);
   assert(ic.count == 4);

   /* but a PATH-QUALIFIED angle include with a stdlib-like basename is a real lib
    * header and is KEPT (matched on the full string, not the basename). */
   c_import_line("#include <thirdparty/string.h>", 7, &ic);
   assert(ic.count == 5);
   assert(strcmp(imports[4], "thirdparty/string.h") == 0);
   assert(sys[4] == 1);

   /* Non-include lines ignored */
   c_import_line("int x = 5;", 8, &ic);
   assert(ic.count == 5);

   c_import_line("// #include \"commented.h\"", 9, &ic);
   assert(ic.count == 5);

   for (int i = 0; i < ic.count; i++)
      free(imports[i]);
}

static void test_c_exports(void)
{
   char *exports[16];
   memset(exports, 0, sizeof(exports));
   export_ctx_t ec = {exports, 0, 16};

   /* Non-static function declaration */
   c_export_line("int agent_execute(sqlite3 *db, const agent_t *agent);", 1, &ec);
   assert(ec.count == 1);
   assert(strcmp(exports[0], "agent_execute") == 0);

   /* Static should be skipped */
   c_export_line("static void helper(void);", 2, &ec);
   assert(ec.count == 1);

   /* Struct declaration */
   c_export_line("struct agent_t {", 3, &ec);
   assert(ec.count == 2);
   assert(strcmp(exports[1], "agent_t") == 0);

   /* Enum declaration */
   c_export_line("enum color { RED, GREEN };", 4, &ec);
   assert(ec.count == 3);
   assert(strcmp(exports[2], "color") == 0);

   /* Typedef */
   c_export_line("typedef struct { int x; } point_t;", 5, &ec);
   assert(ec.count == 4);
   assert(strcmp(exports[3], "point_t") == 0);

   /* Preprocessor lines skipped */
   c_export_line("#define MAX 100", 6, &ec);
   assert(ec.count == 4);

   for (int i = 0; i < ec.count; i++)
      free(exports[i]);
}

static void test_c_definitions(void)
{
   definition_t defs[16];
   memset(defs, 0, sizeof(defs));
   c_def_ctx_t dc = {defs, 0, 16, 0};

   /* Function at indent 0 */
   c_def_line("int main(int argc, char **argv)", 1, &dc);
   assert(dc.count == 1);
   assert(strcmp(defs[0].name, "main") == 0);
   assert(defs[0].line == 1);

   /* Static function still indexed */
   c_def_line("static void helper(void)", 5, &dc);
   assert(dc.count == 2);
   assert(strcmp(defs[1].name, "helper") == 0);
   assert(defs[1].line == 5);

   /* #define macro */
   c_def_line("#define MAX_SIZE 1024", 10, &dc);
   assert(dc.count == 3);
   assert(strcmp(defs[2].name, "MAX_SIZE") == 0);
   assert(defs[2].line == 10);

   /* Struct */
   c_def_line("struct agent_t {", 15, &dc);
   assert(dc.count == 4);
   assert(strcmp(defs[3].name, "agent_t") == 0);

   /* Enum */
   c_def_line("enum color { RED, GREEN };", 20, &dc);
   assert(dc.count == 5);
   assert(strcmp(defs[4].name, "color") == 0);

   /* Typedef */
   c_def_line("typedef unsigned int uint32_t;", 25, &dc);
   assert(dc.count == 6);
   assert(strcmp(defs[5].name, "uint32_t") == 0);

   /* Indented lines skipped (inside function body) */
   c_def_line("   int x = 5;", 30, &dc);
   assert(dc.count == 6);

   /* Block comment tracking */
   c_def_line("/* start of comment", 35, &dc);
   assert(dc.in_block_comment == 1);
   c_def_line("int should_be_skipped(void)", 36, &dc);
   assert(dc.count == 6);
   c_def_line("end of comment */", 37, &dc);
   assert(dc.in_block_comment == 0);

   /* After comment, normal parsing resumes */
   c_def_line("void *after_comment(int x)", 38, &dc);
   assert(dc.count == 7);
   assert(strcmp(defs[6].name, "after_comment") == 0);
}

/* env-var args (getenv/setenv/putenv/unsetenv) are indexed as
 * kind="env_var" so operators can `aimee index find AIMEE_PROFILE`. */
static void test_c_env_var_extraction(void)
{
   definition_t defs[16];
   memset(defs, 0, sizeof(defs));
   c_def_ctx_t dc = {defs, 0, 16, 0};

   c_def_line("   const char *p = getenv(\"AIMEE_PROFILE\");", 42, &dc);
   assert(dc.count == 1);
   assert(strcmp(defs[0].name, "AIMEE_PROFILE") == 0);
   assert(strcmp(defs[0].kind, "env_var") == 0);
   assert(defs[0].line == 42);

   c_def_line("   setenv(\"AIMEE_HOME\", path, 1);", 50, &dc);
   assert(dc.count == 2);
   assert(strcmp(defs[1].name, "AIMEE_HOME") == 0);
   assert(strcmp(defs[1].kind, "env_var") == 0);

   /* Two on one line */
   c_def_line("   if (getenv(\"FOO\") || getenv(\"BAR\")) {}", 60, &dc);
   assert(dc.count == 4);
   assert(strcmp(defs[2].name, "FOO") == 0);
   assert(strcmp(defs[3].name, "BAR") == 0);

   /* Wrapper functions like aimee_getenv() must not match — only the
    * stdlib calls should produce env_var entries. */
   c_def_line("   const char *v = aimee_getenv(\"NOT_AN_ENV_VAR\");", 70, &dc);
   assert(dc.count == 4);

   /* unsetenv + putenv */
   c_def_line("   unsetenv(\"OLD_VAR\");", 80, &dc);
   assert(dc.count == 5);
   assert(strcmp(defs[4].name, "OLD_VAR") == 0);
   c_def_line("   putenv(\"PATH=/tmp\");", 90, &dc);
   assert(dc.count == 6);
   assert(strcmp(defs[5].name, "PATH=/tmp") == 0);

   /* Top-level fn definitions still indexed alongside env-var args */
   c_def_line("int env_using_func(void)", 100, &dc);
   assert(dc.count == 7);
   assert(strcmp(defs[6].name, "env_using_func") == 0);
   /* H0a: granular def kinds — functions are eligible HIGH cross-repo definers; the
    * SDK-prone kinds (macro/typedef) are not (§5 of the precision-hardening proposal). */
   assert(strcmp(defs[6].kind, "function") == 0);

   c_def_line("#define MY_MACRO 1", 110, &dc);
   assert(dc.count == 8 && strcmp(defs[7].name, "MY_MACRO") == 0 &&
          strcmp(defs[7].kind, "macro") == 0);
   c_def_line("typedef int MyInt;", 120, &dc);
   assert(dc.count == 9 && strcmp(defs[8].name, "MyInt") == 0 &&
          strcmp(defs[8].kind, "typedef") == 0);
   c_def_line("struct MyStruct {", 130, &dc);
   assert(dc.count == 10 && strcmp(defs[9].name, "MyStruct") == 0 &&
          strcmp(defs[9].kind, "struct") == 0);
}

/* --- Lua extractor tests --- */

static void test_lua_imports(void)
{
   char *imports[16];
   memset(imports, 0, sizeof(imports));
   import_ctx_t ic = {imports, 0, 16, 0, NULL};

   /* require with double quotes and parens */
   lua_import_line("require(\"socket\")", 1, &ic);
   assert(ic.count == 1);
   assert(strcmp(imports[0], "socket") == 0);

   /* require with single quotes */
   lua_import_line("require('json')", 2, &ic);
   assert(ic.count == 2);
   assert(strcmp(imports[1], "json") == 0);

   /* require without parens */
   lua_import_line("require \"lpeg\"", 3, &ic);
   assert(ic.count == 3);
   assert(strcmp(imports[2], "lpeg") == 0);

   /* local assignment with require */
   lua_import_line("local http = require(\"http\")", 4, &ic);
   assert(ic.count == 4);
   assert(strcmp(imports[3], "http") == 0);

   /* Non-require lines ignored */
   lua_import_line("local x = 5", 5, &ic);
   assert(ic.count == 4);

   lua_import_line("print(\"hello\")", 6, &ic);
   assert(ic.count == 4);

   for (int i = 0; i < ic.count; i++)
      free(imports[i]);
}

static void test_lua_exports(void)
{
   char *exports[16];
   memset(exports, 0, sizeof(exports));
   export_ctx_t ec = {exports, 0, 16};

   /* Module method with dot */
   lua_export_line("function M.connect(host, port)", 1, &ec);
   assert(ec.count == 1);
   assert(strcmp(exports[0], "connect") == 0);

   /* Module method with colon */
   lua_export_line("function M:close()", 2, &ec);
   assert(ec.count == 2);
   assert(strcmp(exports[1], "close") == 0);

   /* Plain function is NOT an export */
   lua_export_line("function helper(x)", 3, &ec);
   assert(ec.count == 2);

   /* Local function is NOT an export */
   lua_export_line("local function internal()", 4, &ec);
   assert(ec.count == 2);

   for (int i = 0; i < ec.count; i++)
      free(exports[i]);
}

static void test_lua_definitions(void)
{
   definition_t defs[16];
   memset(defs, 0, sizeof(defs));
   def_ctx_t dc = {defs, 0, 16, 0};

   /* Global function */
   lua_def_line("function global_func(x, y)", 1, &dc);
   assert(dc.count == 1);
   assert(strcmp(defs[0].name, "global_func") == 0);
   assert(defs[0].line == 1);

   /* Local function */
   lua_def_line("local function helper(a)", 5, &dc);
   assert(dc.count == 2);
   assert(strcmp(defs[1].name, "helper") == 0);
   assert(defs[1].line == 5);

   /* Module method with dot */
   lua_def_line("function M.connect(host)", 10, &dc);
   assert(dc.count == 3);
   assert(strcmp(defs[2].name, "M.connect") == 0);
   assert(defs[2].line == 10);

   /* Module method with colon */
   lua_def_line("function M:close()", 15, &dc);
   assert(dc.count == 4);
   assert(strcmp(defs[3].name, "M:close") == 0);
   assert(defs[3].line == 15);

   /* Non-function lines ignored */
   lua_def_line("local x = 5", 20, &dc);
   assert(dc.count == 4);

   lua_def_line("return M", 25, &dc);
   assert(dc.count == 4);
}

/* --- code_def_end_line: body-span recovery for find_symbol --- */
static void test_def_end_line(void)
{
   /* C: brace-matched span, ignoring an inner block. */
   const char *c = "int a;\n"       /* 1 */
                   "int f(int x)\n" /* 2  <- def start */
                   "{\n"            /* 3 */
                   "   if (x) {\n"  /* 4 */
                   "      x++;\n"   /* 5 */
                   "   }\n"         /* 6 */
                   "   return x;\n" /* 7 */
                   "}\n"            /* 8  <- end */
                   "int g;\n";      /* 9 */
   assert(code_def_end_line(c, 2, ".c") == 8);
   /* A prototype / one-liner (no brace) collapses to its own line. */
   assert(code_def_end_line("int proto(void);\n", 1, ".c") == 1);

   /* Python: indented body, trailing blank line excluded. */
   const char *py = "def f(x):\n"     /* 1  <- def */
                    "    y = x + 1\n" /* 2 */
                    "    return y\n"  /* 3  <- end */
                    "\n"              /* 4  blank */
                    "z = 1\n";        /* 5  dedent */
   assert(code_def_end_line(py, 1, ".py") == 3);

   /* Out-of-range / empty inputs are safe. */
   assert(code_def_end_line("", 1, ".c") == 1);
   assert(code_def_end_line(c, 0, ".c") == 1);
   printf("def_end_line OK\n");
}

int main(void)
{
   test_def_end_line();
   test_c_imports();
   test_c_exports();
   test_c_definitions();
   test_c_env_var_extraction();
   test_lua_imports();
   test_lua_exports();
   test_lua_definitions();
   printf("extractors: all tests passed\n");
   return 0;
}
