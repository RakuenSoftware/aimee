/* Behavior parity for descriptor-owned DB2 code import identities. */
#include "index.h"

#include <assert.h>
#include <stddef.h>
#include <string.h>

#define TEST_OUT_CAP 4098

int db2_support_code_path_import_identity(const char *path, char *out, size_t out_cap);
int db2_support_code_import_identity(const char *importer_path, const char *raw_import, char *out,
                                     size_t out_cap);
int db2_support_code_import_resolves_path(const char *importer_path, const char *raw_import,
                                          const char *target_path);

static const size_t capacities[] = {0, 1, 2, 3, 4, 8, 16, 33, 128, 4096, TEST_OUT_CAP};

static void assert_path_identity(const char *path, size_t cap)
{
   unsigned char legacy[TEST_OUT_CAP];
   unsigned char support[TEST_OUT_CAP];
   memset(legacy, 0xa5, sizeof(legacy));
   memset(support, 0xa5, sizeof(support));
   int legacy_rc = code_path_import_identity(path, (char *)legacy, cap);
   int support_rc = db2_support_code_path_import_identity(path, (char *)support, cap);
   assert(legacy_rc == support_rc);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void assert_import_identity(const char *importer, const char *raw, size_t cap)
{
   unsigned char legacy[TEST_OUT_CAP];
   unsigned char support[TEST_OUT_CAP];
   memset(legacy, 0x5a, sizeof(legacy));
   memset(support, 0x5a, sizeof(support));
   int legacy_rc = code_import_identity(importer, raw, (char *)legacy, cap);
   int support_rc = db2_support_code_import_identity(importer, raw, (char *)support, cap);
   assert(legacy_rc == support_rc);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void test_path_identity(void)
{
   static const char *paths[] = {NULL,
                                 "",
                                 ".",
                                 "./",
                                 ".\\",
                                 "foo",
                                 "./foo",
                                 ".\\foo",
                                 "foo.py",
                                 "pkg/module.py",
                                 "pkg/__init__.py",
                                 "pkg\\nested\\module.py",
                                 "pkg/module.PY",
                                 "pkg.with.dots/file.py",
                                 "../../../module.py"};
   for (size_t path = 0; path < sizeof(paths) / sizeof(paths[0]); path++)
      for (size_t cap = 0; cap < sizeof(capacities) / sizeof(capacities[0]); cap++)
         assert_path_identity(paths[path], capacities[cap]);

   assert(code_path_import_identity("x", NULL, 8) ==
          db2_support_code_path_import_identity("x", NULL, 8));
}

static void test_import_identity(void)
{
   static const char *importers[] = {
       NULL,           "",         "x", "script.py", "app/bill.py", "app/reports/monthly.py",
       "app\\bill.py", "script.PY"};
   static const char *imports[] = {NULL,        "",       ".",        "..",
                                   "...",       "....",   ".dates",   "..dates",
                                   "app.dates", "./path", "..\\path", "foo\\bar"};
   for (size_t importer = 0; importer < sizeof(importers) / sizeof(importers[0]); importer++)
      for (size_t raw = 0; raw < sizeof(imports) / sizeof(imports[0]); raw++)
         for (size_t cap = 0; cap < sizeof(capacities) / sizeof(capacities[0]); cap++)
            assert_import_identity(importers[importer], imports[raw], capacities[cap]);

   assert(code_import_identity("x.py", "x", NULL, 8) ==
          db2_support_code_import_identity("x.py", "x", NULL, 8));
}

static void test_every_non_nul_byte(void)
{
   char value[2] = {0, 0};
   for (unsigned byte = 1; byte <= 255; byte++)
   {
      value[0] = (char)byte;
      assert_path_identity(value, TEST_OUT_CAP);
      assert_import_identity("plain.c", value, TEST_OUT_CAP);
   }
}

static void test_long_boundaries(void)
{
   char long_path[TEST_OUT_CAP + 512];
   memset(long_path, 'a', sizeof(long_path));
   memcpy(long_path + 4, "\\segment/", 9);
   memcpy(long_path + sizeof(long_path) - 4, ".py", 4);
   for (size_t cap = 0; cap < sizeof(capacities) / sizeof(capacities[0]); cap++)
   {
      assert_path_identity(long_path, capacities[cap]);
      assert_import_identity("pkg/importer.py", long_path, capacities[cap]);
   }
}

static void test_resolution_matrix(void)
{
   static const char *importers[] = {
       NULL, "", "app/billing.py", "app/reports/monthly.py", "app\\billing.py", "plain.c"};
   static const char *imports[] = {
       NULL, "", ".dates", "..dates", "app.dates", "app.dates.__init__", "foo\\bar", "./foo"};
   static const char *targets[] = {
       NULL, "", "app/dates.py", "app/dates/__init__.py", "app\\dates.py", "foo/bar", "./foo"};
   for (size_t importer = 0; importer < sizeof(importers) / sizeof(importers[0]); importer++)
      for (size_t raw = 0; raw < sizeof(imports) / sizeof(imports[0]); raw++)
         for (size_t target = 0; target < sizeof(targets) / sizeof(targets[0]); target++)
            assert(code_import_resolves_path(importers[importer], imports[raw], targets[target]) ==
                   db2_support_code_import_resolves_path(importers[importer], imports[raw],
                                                         targets[target]));
}

int main(void)
{
   test_path_identity();
   test_import_identity();
   test_every_non_nul_byte();
   test_long_boundaries();
   test_resolution_matrix();
   return 0;
}
