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

int main(void)
{
   test_find_by_title();
   printf("rules: all tests passed\n");
   return 0;
}
