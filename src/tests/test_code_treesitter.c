/* test_code_treesitter.c: §2 tree-sitter front-end. Built + run ONLY in the opt-in
 * AIMEE_TREESITTER build (it links the fetched runtime + grammars). Parses real source in
 * every supported language and asserts the extracted definition_t set. */
#include "headers/code_treesitter.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int has(const definition_t *d, int n, const char *name, const char *kind)
{
   for (int i = 0; i < n; i++)
      if (strcmp(d[i].name, name) == 0 && strcmp(d[i].kind, kind) == 0)
         return 1;
   return 0;
}

/* Parse `src` as `ext` and assert (name, kind) is among the extracted definitions. */
static void want(const char *ext, const char *src, const char *name, const char *kind)
{
   definition_t d[64];
   int n = code_treesitter_definitions(ext, src, d, 64);
   if (n < 0 || !has(d, n, name, kind))
   {
      printf("  FAIL %s: want %s/%s, got %d defs:", ext, name, kind, n);
      for (int i = 0; i < n; i++)
         printf(" %s/%s", d[i].name, d[i].kind);
      printf("\n");
      assert(0);
   }
}

/* Parse `src` as `ext` and assert a (caller -> callee) call edge was extracted. */
static void wantcall(const char *ext, const char *src, const char *caller, const char *callee)
{
   call_ref_t c[128];
   int n = code_treesitter_calls(ext, src, c, 128);
   int found = 0;
   for (int i = 0; i < n; i++)
      if (strcmp(c[i].caller, caller) == 0 && strcmp(c[i].callee, callee) == 0)
         found = 1;
   if (n < 0 || !found)
   {
      printf("  FAIL %s: want [%s->%s], got %d:", ext, caller, callee, n);
      for (int i = 0; i < n; i++)
         printf(" [%s->%s]", c[i].caller, c[i].callee);
      printf("\n");
      assert(0);
   }
}

int main(void)
{
   printf("test_code_treesitter:\n");

   /* availability gates the dispatch (a representative extension per language). */
   const char *avail[] = {".c",   ".h",   ".cpp", ".cc",   ".hpp",   ".cs", ".py",   ".go",
                          ".js",  ".mjs", ".jsx", ".ts",   ".tsx",   ".rs", ".java", ".rb",
                          ".php", ".lua", ".sh",  ".bash", ".swift", ".kt", ".dart", ".css"};
   for (size_t i = 0; i < sizeof(avail) / sizeof(avail[0]); i++)
      assert(code_treesitter_available(avail[i]));
   assert(!code_treesitter_available(".txt"));
   assert(!code_treesitter_available(".md"));
   assert(!code_treesitter_available(NULL));

   /* --- C --- */
   const char *c_src = "typedef struct Point { int x; } Point;\n"
                       "static int add(int a, int b) { return a + b; }\n"
                       "void greet(const char *who);\n"
                       "int main(void) { return add(1, 2); }\n";
   definition_t d[64];
   int n = code_treesitter_definitions(".c", c_src, d, 64);
   assert(n >= 4);
   want(".c", c_src, "add", "function");
   want(".c", c_src, "greet", "function"); /* prototype */
   want(".c", c_src, "Point", "type");
   for (int i = 0; i < n; i++)
   {
      assert(d[i].line >= 1);
      assert(d[i].line_end >= d[i].line);
   }
   assert(code_treesitter_definitions(".c", c_src, d, 1) == 1); /* bounded */

   /* definitions wrapped in a preprocessor conditional are surfaced (real C/C++ files
    * commonly guard top-level defs with #ifdef/#if). */
   want(".c", "#ifdef X\nint guarded(void){ return 1; }\ntypedef int gint;\n#endif\n", "guarded",
        "function");
   want(".c", "#ifdef X\nint guarded(void){ return 1; }\ntypedef int gint;\n#endif\n", "gint",
        "type");
   want(".c", "#if A\nint a(void){return 0;}\n#else\nint b(void){return 1;}\n#endif\n", "b",
        "function"); /* #else branch too */

   /* --- C++ (incl. a namespaced class — exercises container descent) --- */
   const char *cpp = "int add(int a){return a;}\nclass C{ void m(); };\nstruct S{};\n"
                     "namespace N { class Inner{}; }\n";
   want(".cpp", cpp, "add", "function");
   want(".cpp", cpp, "C", "type");
   want(".cpp", cpp, "S", "type");
   want(".cpp", cpp, "Inner", "type"); /* reached through namespace */

   /* --- C# (types live inside a namespace) --- */
   const char *cs =
       "namespace N { class C { void M(){} } interface I{} enum E{A} record R(int x); }\n"
       "class Top {}\n";
   want(".cs", cs, "C", "type");
   want(".cs", cs, "I", "type");
   want(".cs", cs, "R", "type");
   want(".cs", cs, "Top", "type"); /* top-level, no namespace */
   want(".cs", "#if DEBUG\nclass Dbg { void M(){} }\n#endif\n", "Dbg",
        "type"); /* #if-guarded type */

   /* --- Python (incl. a decorated def) --- */
   const char *py = "def add(a, b):\n    return a + b\n"
                    "class Point:\n    pass\n"
                    "@deco\ndef wrapped():\n    pass\n";
   want(".py", py, "add", "function");
   want(".py", py, "Point", "type");
   want(".py", py, "wrapped", "function");

   /* --- Go --- */
   const char *go = "package main\nfunc Add(a int) int { return a }\n"
                    "func (p *Point) M() {}\ntype Point struct { X int }\n";
   want(".go", go, "Add", "function");
   want(".go", go, "M", "function");
   want(".go", go, "Point", "type");

   /* --- JavaScript (incl. export) --- */
   const char *js = "function add(a){return a}\nclass Point{}\nfunction* gen(){}\n"
                    "export function pub(){}\nexport class Wid{}\n";
   want(".js", js, "add", "function");
   want(".js", js, "Point", "type");
   want(".js", js, "gen", "function");
   want(".js", js, "pub", "function");
   want(".js", js, "Wid", "type");

   /* --- TypeScript --- */
   const char *ts = "function f(){}\nclass C{}\ninterface I{}\ntype T=number\nenum E{A}\n"
                    "export function h(){}\n";
   want(".ts", ts, "f", "function");
   want(".ts", ts, "C", "type");
   want(".ts", ts, "I", "type");
   want(".ts", ts, "T", "type");
   want(".ts", ts, "h", "function");
   want(".tsx", "export function App(){ return null }\n", "App", "function");
   /* arrow / function expressions bound to a const/let or a class field. */
   want(".js", "const f = () => {}\n", "f", "function");
   want(".js", "const g = function(){}\n", "g", "function");
   want(".js", "export const k = () => 2\n", "k", "function");
   want(".js", "class W { fld = () => {} }\n", "fld", "function"); /* class field arrow */
   want(".ts", "export const ah = async (): Promise<void> => {}\n", "ah", "function");
   {
      /* a plain value binding is not a function; an arrow body's locals don't leak. */
      definition_t e[32];
      int m = code_treesitter_definitions(".js", "let x = 1\nconst f = () => { const inner = 1 }\n",
                                          e, 32);
      for (int i = 0; i < m; i++)
         assert(strcmp(e[i].name, "x") != 0 && strcmp(e[i].name, "inner") != 0);
   }

   /* --- Rust --- */
   const char *rs =
       "fn add(a: i32) -> i32 { a }\nstruct Point { x: i32 }\nenum E { A }\ntrait T {}\n";
   want(".rs", rs, "add", "function");
   want(".rs", rs, "Point", "type");
   want(".rs", rs, "E", "type");
   want(".rs", rs, "T", "type");

   /* --- Java (top-level types) --- */
   const char *java = "class C { void m(){} }\ninterface I{}\nenum E{A}\nrecord R(int x){}\n";
   want(".java", java, "C", "type");
   want(".java", java, "I", "type");
   want(".java", java, "R", "type");

   /* --- Ruby --- */
   const char *rb = "def foo; end\nclass C\nend\nmodule M\nend\n";
   want(".rb", rb, "foo", "function");
   want(".rb", rb, "C", "type");
   want(".rb", rb, "M", "type");

   /* --- PHP --- */
   const char *php = "<?php\nfunction f(){}\nclass C{}\ninterface I{}\ntrait Tr{}\n";
   want(".php", php, "f", "function");
   want(".php", php, "C", "type");
   want(".php", php, "Tr", "type");

   /* --- Lua (dotted name preserved) --- */
   const char *lua = "local function f() end\nfunction g() end\nfunction T.m() end\n";
   want(".lua", lua, "g", "function");
   want(".lua", lua, "T.m", "function");

   /* --- Bash --- */
   const char *sh = "foo() { echo hi; }\nfunction bar { echo yo; }\n";
   want(".sh", sh, "foo", "function");
   want(".sh", sh, "bar", "function");

   /* --- Swift --- */
   const char *swift = "func f(){}\nclass C{}\nstruct S{}\nprotocol P{}\n";
   want(".swift", swift, "f", "function");
   want(".swift", swift, "C", "type");
   want(".swift", swift, "P", "type");

   /* --- Kotlin (no name field — the name is the first DIRECT identifier child, which
    * must survive a leading annotation/modifier, a type-parameter list, or an extension
    * receiver that themselves contain identifiers). --- */
   const char *kt = "fun f(){}\nclass C{}\nobject O{}\n"
                    "@Entity class User {}\n"    /* annotation precedes the name */
                    "@Test fun shouldWork(){}\n" /* annotation precedes the name */
                    "fun <T> identity(x: T){}\n" /* type params precede the name */
                    "fun String.shout(){}\n";    /* receiver precedes the name */
   want(".kt", kt, "f", "function");
   want(".kt", kt, "C", "type");
   want(".kt", kt, "O", "type");
   want(".kt", kt, "User", "type");
   want(".kt", kt, "shouldWork", "function");
   want(".kt", kt, "identity", "function");
   want(".kt", kt, "shout", "function");

   /* --- Dart (annotated mixin: name survives the leading annotation) --- */
   const char *dart = "void f(){}\nclass C{}\nenum E{a}\n@sealed mixin Foo {}\n";
   want(".dart", dart, "f", "function");
   want(".dart", dart, "C", "type");
   want(".dart", dart, "Foo", "type");

   /* --- CSS (@keyframes name) --- */
   want(".css", "@keyframes spin { from {} to {} }\n.cls { color: red }\n", "spin", "type");

   /* --- nested members: methods inside a type body are surfaced (the walk descends type
    * bodies but never function bodies), across the OO languages. --- */
   want(".cpp", "class C { void m(){} };\n", "m", "function");
   want(".cs", "namespace N { class C { void M(){} } }\n", "M", "function");
   want(".java", "class C { void m(){} }\n", "m", "function");
   want(".ts", "class C { greet(){} }\n", "greet", "function");
   want(".ts", "export class W { go(){} }\n", "go", "function"); /* exported class's method */
   want(".py", "class C:\n    def m(self):\n        pass\n", "m", "function");
   want(".rs", "impl C { fn m(&self){} }\n", "m", "function"); /* impl method */
   want(".rs", "trait T { fn t(&self); }\n", "t", "function"); /* trait method sig */
   want(".rb", "class C\n  def m; end\nend\n", "m", "function");
   want(".php", "<?php\nclass C { function m(){} }\n", "m", "function");
   want(".swift", "class C { func m(){} }\n", "m", "function");
   want(".dart", "class C { void m(){} }\n", "m", "function");

   /* descending a type body must not double-emit the type or a typedef's struct tag. */
   {
      definition_t e[64];
      int m = code_treesitter_definitions(".c", "typedef struct Point { int x; } Point;\n", e, 64);
      int seen = 0;
      for (int i = 0; i < m; i++)
         if (strcmp(e[i].name, "Point") == 0)
            seen++;
      assert(seen == 1); /* not double-emitted via the struct tag */
   }

   /* a vendored-but-unmapped ext -> -1 (caller falls back to the hand-rolled extractor). */
   assert(code_treesitter_definitions(".md", c_src, d, 64) == -1);

   /* === call edges (caller -> callee), tracking the enclosing function. The callee is
    * the last identifier of the callee expression (`obj.m()` -> m, `a::b()` -> b). === */
   wantcall(".c", "void f(){ g(); obj->m(); }\n", "f", "g");
   wantcall(".c", "void f(){ obj->m(); }\n", "f", "m"); /* method call -> last id */
   wantcall(".py", "def f():\n    g()\n    obj.m()\n", "f", "g");
   wantcall(".py", "def f():\n    obj.m()\n", "f", "m");
   wantcall(".js", "function f(){ g(); a.b.c(); }\n", "f", "g");
   wantcall(".js", "function f(){ a.b.c(); }\n", "f", "c"); /* chained -> last id */
   wantcall(".ts", "function f(){ svc.run(); }\n", "f", "run");
   wantcall(".go", "func f(){ g(); pkg.H() }\n", "f", "H"); /* selector -> field */
   wantcall(".rs", "fn f(){ g(); h::k(); }\n", "f", "k");   /* scoped -> last */
   wantcall(".java", "class C{ void f(){ g(); obj.m(); } }\n", "f", "m");
   wantcall(".cs", "class C{ void F(){ G(); o.M(); } }\n", "F", "M");
   /* generic/template calls resolve to the method name, not a type argument. */
   wantcall(".cs", "class C{ void F(){ provider.GetService<IFoo>(); } }\n", "F", "GetService");
   wantcall(".cs", "class C{ void F(){ var x = new List<Bar>(); } }\n", "F", "List");
   wantcall(".cpp", "void f(){ obj.run<T>(y); }\n", "f", "run");
   wantcall(".rs", "fn f(){ parse::<u32>(); }\n", "f", "parse"); /* turbofish */
   wantcall(".php", "<?php\nfunction f(){ g(); $o->m(); }\n", "f", "m");
   wantcall(".rb", "def f\n  g()\n  obj.m\nend\n", "f", "m"); /* g() w/ parens + obj.m */
   wantcall(".swift", "func f(){ g(); obj.m() }\n", "f", "g");
   wantcall(".lua", "function f() g() end\n", "f", "g");
   wantcall(".py", "top()\n", "", "top"); /* file-scope: empty caller */
   /* caller tracking follows the enclosing function. */
   wantcall(".c", "void outer(){ x(); }\nvoid inner(){ deep(); }\n", "inner", "deep");
   /* Bash/CSS have no useful call extraction -> -1 (caller falls back). */
   {
      call_ref_t cc[8];
      assert(code_treesitter_calls(".css", "a{color:red}\n", cc, 8) == -1);
      assert(code_treesitter_calls(".sh", "f(){ g; }\n", cc, 8) == -1);
      assert(code_treesitter_calls(".md", c_src, cc, 8) == -1); /* unmapped ext */
   }

   printf("  all supported languages extracted (C/C++/C#/Python/Go/JS/TS/Rust/Java/"
          "Ruby/PHP/Lua/Bash/Swift/Kotlin/Dart/CSS)\n");

   /* End-to-end macro preservation through extract_definitions on the REAL
    * tree-sitter path (this binary is built AIMEE_TREESITTER=1). Tree-sitter emits
    * the class + method but NOT the #define; extract_definitions must append the
    * macro pass so a C++ file with a macro + class yields BOTH. A comment-wrapped
    * #define (incl. a multi-line block comment) must not be captured.
    * (cpp-class-method-extraction §3.) */
   {
      const char *src = "#define FOO 1\n"
                        "/*\n"
                        "#define INBLOCK 9\n"
                        "*/\n"
                        "// #define INLINECMT 8\n"
                        "class C { public: void m() {} };\n";
      definition_t defs[64];
      int n = extract_definitions(".cpp", src, defs, 64);
      int macro = 0, type = 0, method = 0, incmt = 0;
      for (int i = 0; i < n; i++)
      {
         if (!strcmp(defs[i].name, "FOO") && !strcmp(defs[i].kind, "macro"))
            macro = 1;
         if (!strcmp(defs[i].name, "C"))
            type = 1;
         if (!strcmp(defs[i].name, "m"))
            method = 1;
         if (!strcmp(defs[i].name, "INBLOCK") || !strcmp(defs[i].name, "INLINECMT"))
            incmt = 1;
      }
      assert(macro);  /* #define preserved via the appended macro pass */
      assert(type);   /* tree-sitter still emits the class */
      assert(method); /* and the bodied method */
      assert(!incmt); /* commented-out #defines (block + line) not captured */
      printf("  extract_definitions merges tree-sitter defs + macros (comment-aware): ok\n");
   }

   printf("ALL PASS\n");
   return 0;
}
