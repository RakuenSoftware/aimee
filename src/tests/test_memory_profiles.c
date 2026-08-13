#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "modules/memory/memory_profile_pack.h"
#include "platform_test_util.h" /* platform_tmpdir: honour TMPDIR, do not leak into /tmp */

static char s_tmpdir[256];

static void write_file(const char *name, const char *content)
{
   char path[512];
   snprintf(path, sizeof(path), "%s/%s", s_tmpdir, name);
   FILE *f = fopen(path, "w");
   assert(f);
   fputs(content, f);
   fclose(f);
}

/* ---- validate ---- */

static void test_validate_good_pack(void)
{
   write_file("good.json", "{\"name\":\"good\",\"description\":\"a valid pack\","
                           "\"validation_rules\":{\"allowed_tiers\":[\"L1\",\"L2\"],"
                           "\"allowed_kinds\":[\"fact\",\"preference\"]},"
                           "\"scope_defaults\":{\"tier\":\"L1\",\"visibility\":\"default\"}}");
   char path[512], err[256] = "";
   snprintf(path, sizeof(path), "%s/good.json", s_tmpdir);
   assert(memory_profile_pack_validate_file(path, err, sizeof(err)) == 0);
   printf("  validate_good_pack: ok\n");
}

static void test_validate_missing_name(void)
{
   write_file("noname.json", "{\"description\":\"missing name\"}");
   char path[512], err[256] = "";
   snprintf(path, sizeof(path), "%s/noname.json", s_tmpdir);
   assert(memory_profile_pack_validate_file(path, err, sizeof(err)) != 0);
   assert(strstr(err, "name") != NULL);
   printf("  validate_missing_name: ok\n");
}

static void test_validate_missing_description(void)
{
   write_file("nodesc.json", "{\"name\":\"nodesc\"}");
   char path[512], err[256] = "";
   snprintf(path, sizeof(path), "%s/nodesc.json", s_tmpdir);
   assert(memory_profile_pack_validate_file(path, err, sizeof(err)) != 0);
   assert(strstr(err, "description") != NULL);
   printf("  validate_missing_description: ok\n");
}

static void test_validate_unknown_key(void)
{
   write_file("badkey.json", "{\"name\":\"badkey\",\"description\":\"d\",\"unknown_field\":\"x\"}");
   char path[512], err[256] = "";
   snprintf(path, sizeof(path), "%s/badkey.json", s_tmpdir);
   assert(memory_profile_pack_validate_file(path, err, sizeof(err)) != 0);
   assert(strstr(err, "unknown") != NULL);
   printf("  validate_unknown_key: ok\n");
}

static void test_validate_invalid_tier(void)
{
   write_file("badtier.json", "{\"name\":\"badtier\",\"description\":\"d\","
                              "\"validation_rules\":{\"allowed_tiers\":[\"L9\"]}}");
   char path[512], err[256] = "";
   snprintf(path, sizeof(path), "%s/badtier.json", s_tmpdir);
   assert(memory_profile_pack_validate_file(path, err, sizeof(err)) != 0);
   assert(strstr(err, "tier") != NULL || strstr(err, "L9") != NULL);
   printf("  validate_invalid_tier: ok\n");
}

/* ---- load ---- */

static void test_load_fields(void)
{
   write_file("mypack.json", "{\"name\":\"mypack\",\"description\":\"My Pack\","
                             "\"validation_rules\":{\"allowed_tiers\":[\"L1\",\"L2\"],"
                             "\"allowed_kinds\":[\"fact\"]},"
                             "\"scope_defaults\":{\"tier\":\"L2\",\"visibility\":\"strict\"}}");
   char err[256] = "";
   memory_profile_pack_t pack;
   assert(memory_profile_pack_load("mypack", &pack, err, sizeof(err)) == 0);
   assert(strcmp(pack.name, "mypack") == 0);
   assert(strcmp(pack.description, "My Pack") == 0);
   assert(pack.allowed_tier_count == 2);
   assert(strcmp(pack.allowed_tiers[0], "L1") == 0);
   assert(strcmp(pack.allowed_tiers[1], "L2") == 0);
   assert(pack.allowed_kind_count == 1);
   assert(strcmp(pack.allowed_kinds[0], "fact") == 0);
   assert(strcmp(pack.default_tier, "L2") == 0);
   assert(strcmp(pack.default_visibility, "strict") == 0);
   printf("  load_fields: ok\n");
}

static void test_load_nonexistent(void)
{
   char err[256] = "";
   memory_profile_pack_t pack;
   assert(memory_profile_pack_load("does_not_exist", &pack, err, sizeof(err)) != 0);
   printf("  load_nonexistent: ok\n");
}

/* ---- list ---- */

static void test_list_packs(void)
{
   /* good.json, noname.json, nodesc.json, badkey.json, badtier.json, mypack.json are present */
   char names[MEMORY_PROFILE_PACK_MAX_LIST][MEMORY_PROFILE_PACK_NAME_LEN];
   int n = memory_profile_pack_list(names, MEMORY_PROFILE_PACK_MAX_LIST);
   assert(n >= 6);
   /* Verify none of the returned names have a .json suffix */
   for (int i = 0; i < n; i++)
      assert(strstr(names[i], ".json") == NULL);
   printf("  list_packs: ok (found %d)\n", n);
}

/* ---- active pack round-trip ---- */

static void test_active_pack_roundtrip(void)
{
   /* Initially no active-pack file → returns "default" */
   char active[MEMORY_PROFILE_PACK_NAME_LEN];
   memory_profile_pack_active(active, sizeof(active));
   assert(strcmp(active, "default") == 0);

   assert(memory_profile_pack_set_active("mypack") == 0);
   memory_profile_pack_active(active, sizeof(active));
   assert(strcmp(active, "mypack") == 0);

   assert(memory_profile_pack_set_active("good") == 0);
   memory_profile_pack_active(active, sizeof(active));
   assert(strcmp(active, "good") == 0);

   printf("  active_pack_roundtrip: ok\n");
}

int main(void)
{
   printf("memory_profile_pack:\n");

   char tmpl[256];
   snprintf(tmpl, sizeof tmpl, "%s/aimee-pack-test-XXXXXX", platform_tmpdir());
   char *dir = mkdtemp(tmpl);
   assert(dir);
   snprintf(s_tmpdir, sizeof(s_tmpdir), "%s", dir);
   setenv("AIMEE_PACK_DIR", s_tmpdir, 1);

   test_validate_good_pack();
   test_validate_missing_name();
   test_validate_missing_description();
   test_validate_unknown_key();
   test_validate_invalid_tier();
   test_load_fields();
   test_load_nonexistent();
   test_list_packs();
   test_active_pack_roundtrip();

   printf("All memory_profile_pack tests passed.\n");
   return 0;
}
