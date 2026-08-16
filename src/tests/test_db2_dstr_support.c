/* Parity tests for descriptor-owned DB2 dynamic-string support. */
#include "dstr.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

void db2_support_dstr_init(dstr_t *s);
void db2_support_dstr_appendf(dstr_t *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
char *db2_support_dstr_steal(dstr_t *s);

static unsigned fail_realloc_count;

void *db2_test_realloc(void *ptr, size_t size)
{
   if (fail_realloc_count)
   {
      fail_realloc_count--;
      return NULL;
   }
   return realloc(ptr, size);
}

_Static_assert(offsetof(dstr_t, data) == 0, "dstr_t.data must remain first");
_Static_assert(offsetof(dstr_t, len) == sizeof(char *), "dstr_t.len ABI drift");
_Static_assert(offsetof(dstr_t, cap) == sizeof(char *) + sizeof(size_t), "dstr_t.cap ABI drift");
_Static_assert(sizeof(dstr_t) == sizeof(char *) + 2 * sizeof(size_t), "dstr_t size drift");

static void assert_same(const dstr_t *monolith, const dstr_t *support)
{
   assert(monolith->len == support->len);
   assert(monolith->cap == support->cap);
   assert((monolith->data == NULL) == (support->data == NULL));
   if (monolith->data)
      assert(memcmp(monolith->data, support->data, monolith->len + 1) == 0);
}

static void test_empty_lifecycle(void)
{
   dstr_t monolith, support;
   dstr_init(&monolith);
   db2_support_dstr_init(&support);
   assert_same(&monolith, &support);
   assert(dstr_steal(&monolith) == NULL);
   assert(db2_support_dstr_steal(&support) == NULL);
   assert_same(&monolith, &support);
}

static void test_format_and_growth_parity(void)
{
   dstr_t monolith, support;
   dstr_init(&monolith);
   db2_support_dstr_init(&support);

   dstr_appendf(&monolith, "# Rules (epoch %d)\n", 17);
   db2_support_dstr_appendf(&support, "# Rules (epoch %d)\n", 17);
   assert_same(&monolith, &support);

   for (int i = 0; i < 128; i++)
   {
      dstr_appendf(&monolith, "%d. %s-%08x\n", i + 1, "policy", (unsigned)i);
      db2_support_dstr_appendf(&support, "%d. %s-%08x\n", i + 1, "policy", (unsigned)i);
      assert_same(&monolith, &support);
   }

   char long_text[4097];
   memset(long_text, 'x', sizeof(long_text) - 1);
   long_text[sizeof(long_text) - 1] = '\0';
   dstr_appendf(&monolith, "%s", long_text);
   db2_support_dstr_appendf(&support, "%s", long_text);
   assert_same(&monolith, &support);

   char *monolith_data = dstr_steal(&monolith);
   char *support_data = db2_support_dstr_steal(&support);
   assert(monolith_data != NULL);
   assert(support_data != NULL);
   assert(strcmp(monolith_data, support_data) == 0);
   assert_same(&monolith, &support);
   free(monolith_data);
   free(support_data);
}

static void test_allocation_failure_preserves_state(void)
{
   dstr_t monolith, support;
   dstr_init(&monolith);
   db2_support_dstr_init(&support);
   dstr_appendf(&monolith, "%s", "seed");
   db2_support_dstr_appendf(&support, "%s", "seed");
   assert_same(&monolith, &support);

   size_t original_len = monolith.len;
   size_t original_cap = monolith.cap;
   fail_realloc_count = 2;
   dstr_appendf(&monolith, "%01024d", 7);
   db2_support_dstr_appendf(&support, "%01024d", 7);
   assert(fail_realloc_count == 0);
   assert_same(&monolith, &support);
   assert(monolith.len == original_len);
   assert(monolith.cap == original_cap);
   assert(strcmp(monolith.data, "seed") == 0);

   free(dstr_steal(&monolith));
   free(db2_support_dstr_steal(&support));
}

int main(void)
{
   test_empty_lifecycle();
   test_format_and_growth_parity();
   test_allocation_failure_preserves_state();
   return 0;
}
