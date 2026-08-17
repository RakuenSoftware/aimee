/* Replay the generated DB2 fallback parser beside the authoritative monolith. */
#include "index.h"
#define AIMEE_DB2_EXTRACTOR_NO_PUBLIC_PROTOTYPES 1
#include "../modules/db2/support/db2_extractors.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int db2_support_extract_imports_sys(const char *, const char *, char **, int *, int);
int db2_support_extract_exports(const char *, const char *, char **, int);
int db2_support_extract_routes(const char *, const char *, char **, int);
int db2_support_extract_definitions(const char *, const char *, db2_definition_t *, int);
int db2_support_extract_calls(const char *, const char *, db2_call_ref_t *, int);

_Static_assert(sizeof(db2_definition_t) == sizeof(definition_t), "definition ABI size");
_Static_assert(offsetof(db2_definition_t, name) == offsetof(definition_t, name),
               "definition name ABI");
_Static_assert(offsetof(db2_definition_t, kind) == offsetof(definition_t, kind),
               "definition kind ABI");
_Static_assert(offsetof(db2_definition_t, line) == offsetof(definition_t, line),
               "definition line ABI");
_Static_assert(offsetof(db2_definition_t, line_end) == offsetof(definition_t, line_end),
               "definition end ABI");
_Static_assert(sizeof(db2_call_ref_t) == sizeof(call_ref_t), "call ABI size");
_Static_assert(offsetof(db2_call_ref_t, caller) == offsetof(call_ref_t, caller), "caller ABI");
_Static_assert(offsetof(db2_call_ref_t, callee) == offsetof(call_ref_t, callee), "callee ABI");
_Static_assert(offsetof(db2_call_ref_t, line) == offsetof(call_ref_t, line), "call line ABI");

typedef struct
{
   const char *ext;
   const char *content;
} fixture_t;

static const fixture_t FIXTURES[] = {
    {".c", "#include \"local.h\"\n#include <thirdparty/string.h>\n#define LIMIT 4\n"
           "int exported(void) { helper(); return LIMIT; }\n"},
    {".js", "import thing from './thing.js';\nexport function handler() { callMe(); }\n"
            "app.get('/health', handler);\n"},
    {".ts", "import {Thing} from './thing';\nexport interface Shape {}\n"
            "export function draw() { paint(); }\n"},
    {".py", "from app import dates\n\ndef bill():\n    return dates.days()\n"},
    {".go", "package demo\nimport \"example.org/lib\"\nfunc Exported() { helper() }\n"},
    {".cs", "using System.Text;\npublic class Widget { public void Run() { Tick(); } }\n"
            "app.MapGet(\"/ready\", Ready);\n"},
    {".sh", "source ./helpers.sh\nrun_job() { helper; }\n"},
    {".css", "@import \"theme.css\";\n.card { color: red; }\n"},
    {".dart", "import 'package:demo/a.dart';\nclass Widget {}\nvoid run() { tick(); }\n"},
    {".lua", "local dep = require('demo.dep')\nfunction public.run() helper() end\n"},
    {".java", "import demo.Dep;\npublic class App { public void run() { tick(); } }\n"
              "@GetMapping(\"/ready\")\n"},
    {".rs", "use crate::dep;\npub struct Item;\npub fn run() { tick(); }\n"},
    {".rb", "require 'demo/dep'\nclass App\n  def run\n    tick()\n  end\nend\n"
            "get '/ready' do\nend\n"},
    {".kt", "import demo.Dep\nclass App { fun run() { tick() } }\n"
            "@GetMapping(\"/ready\")\n"},
    {".swift", "import Foundation\npublic class App { func run() { tick() } }\n"},
    {".php", "<?php\nrequire 'demo.php';\nclass App { public function run() { tick(); } }\n"},
    {".unknown", "nothing()\n"},
};

static void compare_strings(const fixture_t *fixture,
                            int (*legacy)(const char *, const char *, char **, int),
                            int (*support)(const char *, const char *, char **, int))
{
   char *left[64] = {0};
   char *right[64] = {0};
   int nl = legacy(fixture->ext, fixture->content, left, 64);
   int nr = support(fixture->ext, fixture->content, right, 64);
   assert(nl == nr);
   for (int i = 0; i < nl; i++)
   {
      assert(left[i] && right[i]);
      assert(strcmp(left[i], right[i]) == 0);
      free(left[i]);
      free(right[i]);
   }
}

static void compare_imports(const fixture_t *fixture)
{
   char *left[64] = {0};
   char *right[64] = {0};
   int left_sys[64] = {0};
   int right_sys[64] = {0};
   int nl = extract_imports_sys(fixture->ext, fixture->content, left, left_sys, 64);
   int nr = db2_support_extract_imports_sys(fixture->ext, fixture->content, right, right_sys, 64);
   assert(nl == nr);
   for (int i = 0; i < nl; i++)
   {
      assert(left[i] && right[i]);
      assert(strcmp(left[i], right[i]) == 0);
      assert(left_sys[i] == right_sys[i]);
      free(left[i]);
      free(right[i]);
   }
}

static void compare_definitions(const fixture_t *fixture)
{
   definition_t left[64] = {0};
   db2_definition_t right[64] = {0};
   int nl = extract_definitions(fixture->ext, fixture->content, left, 64);
   int nr = db2_support_extract_definitions(fixture->ext, fixture->content, right, 64);
   assert(nl == nr);
   for (int i = 0; i < nl; i++)
   {
      assert(strcmp(left[i].name, right[i].name) == 0);
      assert(strcmp(left[i].kind, right[i].kind) == 0);
      assert(left[i].line == right[i].line);
      assert(left[i].line_end == right[i].line_end);
   }
}

static void compare_calls(const fixture_t *fixture)
{
   call_ref_t left[64] = {0};
   db2_call_ref_t right[64] = {0};
   int nl = extract_calls(fixture->ext, fixture->content, left, 64);
   int nr = db2_support_extract_calls(fixture->ext, fixture->content, right, 64);
   assert(nl == nr);
   for (int i = 0; i < nl; i++)
   {
      assert(strcmp(left[i].caller, right[i].caller) == 0);
      assert(strcmp(left[i].callee, right[i].callee) == 0);
      assert(left[i].line == right[i].line);
   }
}

int main(void)
{
   for (size_t i = 0; i < sizeof(FIXTURES) / sizeof(FIXTURES[0]); i++)
   {
      compare_imports(&FIXTURES[i]);
      compare_strings(&FIXTURES[i], extract_exports, db2_support_extract_exports);
      compare_strings(&FIXTURES[i], extract_routes, db2_support_extract_routes);
      compare_definitions(&FIXTURES[i]);
      compare_calls(&FIXTURES[i]);
   }
   return 0;
}
