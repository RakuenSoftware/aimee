#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee.h"
#include "db1_client/db1.h"
#include "modules/kb/c/kb_store.h"
#include "db_postgres.h"
#include "modules/kb/c/kb_store_test_shim.h"
#include "../modules/kb/c/kb_store_internal.h"

static void seed_rule(const char *polarity, const char *title, const char *description, int weight)
{
   char sql[1024];
   snprintf(sql, sizeof(sql),
            "INSERT INTO rules (polarity, title, description, weight, created_at, updated_at) "
            "VALUES ('%s', '%s', '%s', %d, '2025-01-01', '2025-01-01')",
            polarity, title, description, weight);
   (void)aimee_pg_exec(kb_store_conn(), sql, NULL, 0);
}

static void setup(void)
{
   kb_store_test_shim_open();
}

static void teardown(void)
{
   kb_store_test_shim_close();
}

static void test_insert_and_list(void)
{
   setup();
   seed_rule("positive", "Test Rule", "A test", 75);

   rule_t rules[10];
   int n = kb_store_rules_list(rules, 10);
   assert(n == 1);
   assert(strcmp(rules[0].polarity, "positive") == 0);
   assert(rules[0].weight == 75);

   teardown();
}

static void test_find_by_title(void)
{
   setup();
   seed_rule("negative", "No Force Push", "Avoid force push", 80);

   rule_t r;
   int rc = kb_store_rules_find_by_title("no force push", &r);
   assert(rc == 0);
   assert(r.weight == 80);

   rc = kb_store_rules_find_by_title("nonexistent", &r);
   assert(rc == -1);

   teardown();
}

static void test_generate(void)
{
   setup();
   seed_rule("positive", "Rule A", "Do this", 80);
   seed_rule("negative", "Rule B", "Avoid that", 60);

   char *md = kb_store_rules_generate();
   assert(md != NULL);
   assert(strstr(md, "# Rules") != NULL);
   assert(strstr(md, "Do this") != NULL);
   assert(strstr(md, "Avoid that") != NULL);
   free(md);

   teardown();
}

static void test_tier_label(void)
{
   assert(strcmp(kb_store_rules_tier(80), "Rule") == 0);
   assert(strcmp(kb_store_rules_tier(60), "Inclination") == 0);
   assert(strcmp(kb_store_rules_tier(30), "Archived") == 0);
}

static void test_polarity_symbol(void)
{
   assert(kb_store_rules_polarity_symbol("positive") == '+');
   assert(kb_store_rules_polarity_symbol("negative") == '-');
   assert(kb_store_rules_polarity_symbol("principle") == '~');
}

int main(void)
{
   test_insert_and_list();
   test_find_by_title();
   test_generate();
   test_tier_label();
   test_polarity_symbol();
   printf("rules: all tests passed\n");
   return 0;
}
