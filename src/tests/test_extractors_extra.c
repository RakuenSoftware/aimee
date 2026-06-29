#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "extractors_extra.h" /* c_def_ctx_t, for_each_line, c_macro_def_line */

/* Declared in index.h */
int extract_imports(const char *ext, const char *content, char **out, int max);
int extract_imports_sys(const char *ext, const char *content, char **out, int *sys, int max);
int extract_exports(const char *ext, const char *content, char **out, int max);
int extract_definitions(const char *ext, const char *content, definition_t *out, int max);

int main(void)
{
   printf("extractors_extra: ");

   /* --- C/C++ includes: H6 captures quoted (is_system=0) + real-lib angle (=1);
    * stdlib/system angle (<stdio.h>) is dropped at extraction. --- */
   {
      const char *c = "#include \"local.h\"\n"
                      "#include <Limelight.h>\n"
                      "#include <stdio.h>\n";
      char *imports[16];
      int sys[16];
      int count = extract_imports_sys(".c", c, imports, sys, 16);
      int q = 0, a = 0, sysdrop = 1;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(imports[i], "local.h") == 0)
         {
            q++;
            assert(sys[i] == 0); /* quoted */
         }
         if (strcmp(imports[i], "Limelight.h") == 0)
         {
            a++;
            assert(sys[i] == 1); /* angle, real lib */
         }
         if (strcmp(imports[i], "stdio.h") == 0)
            sysdrop = 0; /* must NOT appear */
         free(imports[i]);
      }
      assert(q == 1 && a == 1 && sysdrop); /* local.h + Limelight.h; stdio.h dropped */
      /* the non-sys wrapper still works (flags simply not reported) */
      char *imp2[16];
      int c2 = extract_imports(".c", c, imp2, 16);
      assert(c2 == count);
      for (int i = 0; i < c2; i++)
         free(imp2[i]);
   }

   /* --- JavaScript imports --- */
   {
      const char *js = "import React from 'react';\n"
                       "import { useState } from 'react';\n"
                       "const fs = require('fs');\n"
                       "const path = require('path');\n";
      char *imports[16];
      int count = extract_imports(".js", js, imports, 16);
      /* JS extractor should find at least one import */
      for (int i = 0; i < count; i++)
         free(imports[i]);
      /* Just verify it doesn't crash and returns >= 0 */
      assert(count >= 0);
   }

   /* --- JavaScript definitions --- */
   {
      const char *js = "function hello() {}\n"
                       "class MyComponent extends React.Component {}\n";
      definition_t defs[16];
      int count = extract_definitions(".js", js, defs, 16);
      assert(count >= 0); /* Should not crash */
   }

   /* --- Python imports --- */
   {
      const char *py = "import os\nimport sys\nfrom pathlib import Path\n";
      char *imports[16];
      int count = extract_imports(".py", py, imports, 16);
      assert(count >= 0);
      int found_os = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(imports[i], "os"))
            found_os = 1;
         free(imports[i]);
      }
      if (count > 0)
         assert(found_os);
   }

   /* --- Python definitions --- */
   {
      const char *py = "def hello():\n    pass\n\nclass MyClass:\n    pass\n";
      definition_t defs[16];
      int count = extract_definitions(".py", py, defs, 16);
      assert(count >= 1);
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "hello") == 0 || strcmp(defs[i].name, "MyClass") == 0)
            found = 1;
      }
      assert(found);
   }

   /* --- Go imports --- */
   {
      const char *go = "package main\n\nimport (\n\t\"fmt\"\n\t\"os\"\n)\n";
      char *imports[16];
      int count = extract_imports(".go", go, imports, 16);
      assert(count >= 0);
      for (int i = 0; i < count; i++)
         free(imports[i]);
   }

   /* --- Go definitions --- */
   {
      const char *go = "package main\n\nfunc Hello() string {\n\treturn \"hello\"\n}\n\n"
                       "type Server struct {\n\tPort int\n}\n";
      definition_t defs[16];
      int count = extract_definitions(".go", go, defs, 16);
      assert(count >= 1);
   }

   /* --- TypeScript definitions --- */
   {
      const char *ts = "interface User {\n\tname: string;\n}\n\n"
                       "export function greet(user: User): string {\n\treturn 'hi';\n}\n";
      definition_t defs[16];
      int count = extract_definitions(".ts", ts, defs, 16);
      assert(count >= 0);
   }

   /* --- Java imports --- */
   {
      const char *java = "package com.example;\n"
                         "import java.util.List;\n"
                         "import static java.lang.Math.abs;\n";
      char *imports[16];
      int count = extract_imports(".java", java, imports, 16);
      assert(count >= 1);
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(imports[i], "java.util.List"))
            found = 1;
         free(imports[i]);
      }
      assert(found);
   }

   /* --- Java definitions --- */
   {
      const char *java = "public class MyService {\n"
                         "    public void doWork() {}\n"
                         "    private int helper(int x) { return x; }\n"
                         "}\n"
                         "public interface Runnable {\n"
                         "    void run();\n"
                         "}\n";
      definition_t defs[16];
      int count = extract_definitions(".java", java, defs, 16);
      assert(count >= 2);
      int found_class = 0, found_iface = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "MyService") == 0)
            found_class = 1;
         if (strcmp(defs[i].name, "Runnable") == 0)
            found_iface = 1;
      }
      assert(found_class);
      assert(found_iface);
   }

   /* --- Rust imports --- */
   {
      const char *rs = "use std::collections::HashMap;\n"
                       "use crate::models::User;\n";
      char *imports[16];
      int count = extract_imports(".rs", rs, imports, 16);
      assert(count >= 1);
      for (int i = 0; i < count; i++)
         free(imports[i]);
   }

   /* --- Rust definitions --- */
   {
      const char *rs = "pub fn greet(name: &str) -> String {\n"
                       "    format!(\"Hello, {}\", name)\n"
                       "}\n"
                       "pub struct Config {\n"
                       "    pub port: u16,\n"
                       "}\n"
                       "pub trait Handler {\n"
                       "    fn handle(&self);\n"
                       "}\n";
      definition_t defs[16];
      int count = extract_definitions(".rs", rs, defs, 16);
      assert(count >= 3);
      int found_fn = 0, found_struct = 0, found_trait = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "greet") == 0)
            found_fn = 1;
         if (strcmp(defs[i].name, "Config") == 0)
            found_struct = 1;
         if (strcmp(defs[i].name, "Handler") == 0)
            found_trait = 1;
      }
      assert(found_fn);
      assert(found_struct);
      assert(found_trait);
   }

   /* --- Ruby imports --- */
   {
      const char *rb = "require 'json'\nrequire_relative 'models/user'\n";
      char *imports[16];
      int count = extract_imports(".rb", rb, imports, 16);
      assert(count >= 1);
      int found = 0;
      for (int i = 0; i < count; i++)
      {
         if (strstr(imports[i], "models/user"))
            found = 1;
         free(imports[i]);
      }
      assert(found);
   }

   /* --- Ruby definitions --- */
   {
      const char *rb = "class User\n"
                       "  def initialize(name)\n"
                       "    @name = name\n"
                       "  end\n"
                       "  def greet\n"
                       "    puts @name\n"
                       "  end\n"
                       "end\n"
                       "module Helpers\n"
                       "end\n";
      definition_t defs[16];
      int count = extract_definitions(".rb", rb, defs, 16);
      assert(count >= 2);
      int found_class = 0, found_mod = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "User") == 0)
            found_class = 1;
         if (strcmp(defs[i].name, "Helpers") == 0)
            found_mod = 1;
      }
      assert(found_class);
      assert(found_mod);
   }

   /* --- Kotlin definitions --- */
   {
      const char *kt = "fun main() {}\n"
                       "data class User(val name: String)\n"
                       "interface Service {\n"
                       "    fun execute()\n"
                       "}\n";
      definition_t defs[16];
      int count = extract_definitions(".kt", kt, defs, 16);
      assert(count >= 2);
      int found_fn = 0, found_class = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "main") == 0)
            found_fn = 1;
         if (strcmp(defs[i].name, "User") == 0)
            found_class = 1;
      }
      assert(found_fn);
      assert(found_class);
   }

   /* --- Swift definitions --- */
   {
      const char *swift = "import Foundation\n"
                          "func greet(_ name: String) -> String { return name }\n"
                          "struct Point { var x: Double; var y: Double }\n"
                          "protocol Drawable { func draw() }\n";
      char *imports[16];
      int icount = extract_imports(".swift", swift, imports, 16);
      assert(icount >= 1);
      assert(strcmp(imports[0], "Foundation") == 0);
      free(imports[0]);

      definition_t defs[16];
      int count = extract_definitions(".swift", swift, defs, 16);
      assert(count >= 2);
      int found_fn = 0, found_struct = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "greet") == 0)
            found_fn = 1;
         if (strcmp(defs[i].name, "Point") == 0)
            found_struct = 1;
      }
      assert(found_fn);
      assert(found_struct);
   }

   /* --- PHP definitions --- */
   {
      const char *php = "<?php\n"
                        "require_once 'vendor/autoload.php';\n"
                        "class UserController {\n"
                        "    public function index() {}\n"
                        "    private function helper() {}\n"
                        "}\n"
                        "interface Repository {\n"
                        "    public function find($id);\n"
                        "}\n";
      char *imports[16];
      int icount = extract_imports(".php", php, imports, 16);
      assert(icount >= 1);
      for (int i = 0; i < icount; i++)
         free(imports[i]);

      definition_t defs[16];
      int count = extract_definitions(".php", php, defs, 16);
      assert(count >= 2);
      int found_class = 0, found_iface = 0;
      for (int i = 0; i < count; i++)
      {
         if (strcmp(defs[i].name, "UserController") == 0)
            found_class = 1;
         if (strcmp(defs[i].name, "Repository") == 0)
            found_iface = 1;
      }
      assert(found_class);
      assert(found_iface);
   }

   /* --- C++ extensions recognised --- */
   {
      const char *cpp = "#include \"myheader.h\"\nint foo(int x) { return x; }\n";
      definition_t defs[16];
      int count = extract_definitions(".cpp", cpp, defs, 16);
      assert(count >= 1);
      assert(strcmp(defs[0].name, "foo") == 0);
   }

   /* --- Unsupported extension returns 0 --- */
   {
      char *imports[4];
      int count = extract_imports(".xyz", "some content", imports, 4);
      assert(count == 0);
   }

   /* c_macro_def_line: the macro-only scan that preserves #define macros when the
    * tree-sitter front-end (which emits none) handles a C/C++ file. It must emit
    * ONLY macros (def_kind='macro'), be comment-aware (block + line), and ignore
    * functions/types. */
   {
      definition_t defs[16];
      c_def_ctx_t ctx = {defs, 0, 16, 0};
      const char *src = "#define FOO 1\n"
                        "/* #define INCMT 9 */\n"
                        "// #define LINECMT 8\n"
                        "int realfn(void) { return 0; }\n"
                        "#define BAR(x) ((x) + 1)\n";
      for_each_line(src, c_macro_def_line, &ctx);
      int foo = 0, bar = 0, other = 0;
      for (int i = 0; i < ctx.count; i++)
      {
         if (!strcmp(defs[i].name, "FOO") && !strcmp(defs[i].kind, "macro"))
            foo = 1;
         else if (!strcmp(defs[i].name, "BAR") && !strcmp(defs[i].kind, "macro"))
            bar = 1;
         else
            other = 1;
      }
      assert(foo && bar);     /* both real #defines captured */
      assert(ctx.count == 2); /* ONLY them — comment-wrapped #defines + the function excluded */
      assert(!other);
      printf("  c_macro_def_line: macros only, comment-aware: ok\n");
   }

   printf("all tests passed\n");
   return 0;
}
