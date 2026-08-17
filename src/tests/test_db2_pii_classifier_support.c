/* ABI, behavior, and registered-provider parity for DB2's PII classifiers. */
#include "modules/memory/memory_pii_gate.h"

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

void db2_support_memory_pii_register_turn_classifier(memory_pii_turn_classifier_fn classifier);
int db2_support_memory_pii_turn_requests_sensitive(const char *turn_text);
int db2_support_memory_pii_rel_sensitivity(const char *rel_type);
void db2_support_memory_pii_register_sensitivity_batch(memory_pii_sensitivity_batch_fn classifier);
int db2_support_memory_pii_rel_sensitivity_batch(const char *const *rel_types, int count,
                                                 rel_sensitivity_t *out);

_Static_assert((int)SENS_NORMAL == 0, "normal sensitivity ABI drifted");
_Static_assert((int)SENS_PII == 1, "PII sensitivity ABI drifted");
_Static_assert((int)SENS_SECRET == 2, "secret sensitivity ABI drifted");
_Static_assert(sizeof(rel_sensitivity_t) == sizeof(int),
               "relationship sensitivity calling convention is no longer int-sized");

static void reset_classifiers(void)
{
   memory_pii_register_turn_classifier(NULL);
   db2_support_memory_pii_register_turn_classifier(NULL);
   memory_pii_register_sensitivity_batch(NULL);
   db2_support_memory_pii_register_sensitivity_batch(NULL);
}

static void assert_turn(const char *text)
{
   assert(memory_pii_turn_requests_sensitive(text) ==
          db2_support_memory_pii_turn_requests_sensitive(text));
}

static void test_local_turn_classifier(void)
{
   static const char *turns[] = {
       NULL,
       "",
       "weather",
       "what is my address?",
       "WHAT IS MY EMAIL?",
       "birthday",
       "birth date",
       "date of birth",
       "born on",
       "password",
       "passphrase",
       "credential",
       "secret",
       "api key",
       "ssn",
       "social security",
       "where do i live",
       "where i live",
       "my number",
       "home ip",
       "ip address",
       "compass word",
       "emailing",
       "PHONE",
   };
   reset_classifiers();
   for (size_t i = 0; i < sizeof(turns) / sizeof(turns[0]); i++)
      assert_turn(turns[i]);

   char byte_text[2] = {0, 0};
   for (unsigned byte = 1; byte <= 255; byte++)
   {
      byte_text[0] = (char)byte;
      assert_turn(byte_text);
   }

   char long_turn[2048];
   memset(long_turn, 'x', sizeof(long_turn));
   memcpy(long_turn + sizeof(long_turn) - 12, " API KEY!!", 11);
   long_turn[sizeof(long_turn) - 1] = '\0';
   assert_turn(long_turn);
}

static int turn_callback_mode;
static int turn_callback_calls;

static int turn_callback(const char *text, int *requests_sensitive)
{
   assert(text && strcmp(text, "provider turn") == 0);
   turn_callback_calls++;
   *requests_sensitive = turn_callback_mode;
   return turn_callback_mode == 7 ? -1 : 0;
}

static void test_registered_turn_classifier(void)
{
   for (int mode = -1; mode <= 7; mode++)
   {
      turn_callback_mode = mode;
      turn_callback_calls = 0;
      memory_pii_register_turn_classifier(turn_callback);
      int legacy = memory_pii_turn_requests_sensitive("provider turn");
      assert(turn_callback_calls == 1);
      turn_callback_calls = 0;
      db2_support_memory_pii_register_turn_classifier(turn_callback);
      int support = db2_support_memory_pii_turn_requests_sensitive("provider turn");
      assert(turn_callback_calls == 1);
      assert(legacy == support);
   }
   reset_classifiers();
}

static void assert_sensitivity(const char *relation)
{
   assert((int)memory_pii_rel_sensitivity(relation) ==
          db2_support_memory_pii_rel_sensitivity(relation));
}

static void test_relation_classifier(void)
{
   static const char *relations[] = {
       NULL,
       "",
       "works_for",
       "also_known_as",
       "age",
       "home_password",
       "api_key",
       "ssn",
       "home_address",
       "totally_unknown_rel",
       "Favorite Food",
       "CREDIT CARD",
       "private-key",
       "passport_number",
       "email_address",
       "token_owner",
       "credential_hint",
   };
   for (size_t i = 0; i < sizeof(relations) / sizeof(relations[0]); i++)
      assert_sensitivity(relations[i]);

   int seed_count = rel_types_seed_count();
   for (int i = 0; i < seed_count; i++)
   {
      const rel_type_def_t *definition = rel_types_seed_at(i);
      assert(definition);
      assert_sensitivity(definition->rel_type);
   }

   char byte_relation[2] = {0, 0};
   for (unsigned byte = 1; byte <= 255; byte++)
   {
      byte_relation[0] = (char)byte;
      assert_sensitivity(byte_relation);
   }

   char long_relation[256];
   memset(long_relation, 'x', sizeof(long_relation));
   memcpy(long_relation + 20, "_password_", 10);
   long_relation[sizeof(long_relation) - 1] = '\0';
   assert_sensitivity(long_relation);
}

static int batch_mode;
static int batch_calls;

static int batch_callback(const char *const *relations, int count, rel_sensitivity_t *out)
{
   assert(relations && count == 5);
   batch_calls++;
   for (int i = 0; i < count; i++)
      out[i] = (rel_sensitivity_t)(batch_mode + i);
   return batch_mode == 7 ? -1 : 0;
}

static void assert_batch_equal(const char *const *relations, int count)
{
   rel_sensitivity_t legacy[8];
   rel_sensitivity_t support[8];
   memset(legacy, 0xa5, sizeof(legacy));
   memset(support, 0xa5, sizeof(support));
   int legacy_rc = memory_pii_rel_sensitivity_batch(relations, count, legacy);
   int support_rc = db2_support_memory_pii_rel_sensitivity_batch(relations, count, support);
   assert(legacy_rc == support_rc);
   assert(memcmp(legacy, support, sizeof(legacy)) == 0);
}

static void test_local_batch_classifier(void)
{
   static const char *relations[] = {"works_for", "age", "home_password", NULL, "unknown"};
   reset_classifiers();
   assert_batch_equal(relations, 5);
   assert(memory_pii_rel_sensitivity_batch(NULL, 5, (rel_sensitivity_t[5]){0}) ==
          db2_support_memory_pii_rel_sensitivity_batch(NULL, 5, (rel_sensitivity_t[5]){0}));
   assert(memory_pii_rel_sensitivity_batch(relations, 0, (rel_sensitivity_t[5]){0}) ==
          db2_support_memory_pii_rel_sensitivity_batch(relations, 0, (rel_sensitivity_t[5]){0}));
   assert(memory_pii_rel_sensitivity_batch(relations, 5, NULL) ==
          db2_support_memory_pii_rel_sensitivity_batch(relations, 5, NULL));
}

static void test_registered_batch_classifier(void)
{
   static const char *relations[] = {"a", "b", "c", "d", "e"};
   for (batch_mode = -1; batch_mode <= 7; batch_mode++)
   {
      batch_calls = 0;
      memory_pii_register_sensitivity_batch(batch_callback);
      rel_sensitivity_t legacy[5] = {0};
      int legacy_rc = memory_pii_rel_sensitivity_batch(relations, 5, legacy);
      assert(batch_calls == 1);
      batch_calls = 0;
      db2_support_memory_pii_register_sensitivity_batch(batch_callback);
      rel_sensitivity_t support[5] = {0};
      int support_rc = db2_support_memory_pii_rel_sensitivity_batch(relations, 5, support);
      assert(batch_calls == 1);
      assert(legacy_rc == support_rc);
      assert(memcmp(legacy, support, sizeof(legacy)) == 0);
   }
   reset_classifiers();
}

int main(void)
{
   test_local_turn_classifier();
   test_registered_turn_classifier();
   test_relation_classifier();
   test_local_batch_classifier();
   test_registered_batch_classifier();
   return 0;
}
