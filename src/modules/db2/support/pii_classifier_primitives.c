#include "db2_pii_classifier.h"
#include "db2_rel_seed.h"
#include "db2_rel_type_helpers.h"

#include <ctype.h>
#include <string.h>

static int ci_contains(const char *haystack, const char *needle)
{
   size_t haystack_len = strlen(haystack);
   size_t needle_len = strlen(needle);
   if (needle_len == 0)
      return 1;
   if (needle_len > haystack_len)
      return 0;
   for (size_t i = 0; i + needle_len <= haystack_len; i++)
   {
      size_t matched = 0;
      while (matched < needle_len && tolower((unsigned char)haystack[i + matched]) ==
                                         tolower((unsigned char)needle[matched]))
         matched++;
      if (matched == needle_len)
         return 1;
   }
   return 0;
}

static db2_memory_pii_turn_classifier_fn turn_classifier;

void memory_pii_register_turn_classifier(db2_memory_pii_turn_classifier_fn classifier)
{
   turn_classifier = classifier;
}

int memory_pii_turn_requests_sensitive(const char *turn_text)
{
   if (!turn_text || !turn_text[0])
      return 0;
   if (turn_classifier)
   {
      int requests_sensitive = 0;
      if (turn_classifier(turn_text, &requests_sensitive) != 0)
         return 0;
      return requests_sensitive ? 1 : 0;
   }
   static const char *cues[] = {
       "address",       "phone",     "email",    "birthday",        "birth date",
       "date of birth", "born on",   "password", "passphrase",      "credential",
       "secret",        "api key",   "ssn",      "social security", "where do i live",
       "where i live",  "my number", "home ip",  "ip address",
   };
   for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); i++)
      if (ci_contains(turn_text, cues[i]))
         return 1;
   return 0;
}

static int unknown_rel_sensitivity(const char *normalized)
{
   static const char *secret_tokens[] = {
       "password",   "passwd",      "passphrase", "secret",  "api_key", "apikey",
       "access_key", "private_key", "privkey",    "ssh_key", "token",   "credential",
   };
   for (size_t i = 0; i < sizeof(secret_tokens) / sizeof(secret_tokens[0]); i++)
      if (ci_contains(normalized, secret_tokens[i]))
         return DB2_PII_CLASSIFIER_SENS_SECRET;

   static const char *pii_tokens[] = {
       "ssn",
       "social_security",
       "passport",
       "credit_card",
       "creditcard",
       "card_number",
       "cvv",
       "bank_account",
       "account_number",
       "routing_number",
       "tax_id",
       "national_id",
       "drivers_license",
       "license_number",
       "phone",
       "email",
       "date_of_birth",
       "birthdate",
       "dob",
       "home_address",
       "street_address",
   };
   for (size_t i = 0; i < sizeof(pii_tokens) / sizeof(pii_tokens[0]); i++)
      if (ci_contains(normalized, pii_tokens[i]))
         return DB2_PII_CLASSIFIER_SENS_PII;

   return DB2_PII_CLASSIFIER_SENS_NORMAL;
}

int memory_pii_rel_sensitivity(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return DB2_PII_CLASSIFIER_SENS_NORMAL;
   char normalized[DB2_REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, normalized, sizeof(normalized));
   const db2_rel_seed_def_t *definition = normalized[0] ? rel_types_seed_lookup(normalized) : NULL;
   if (definition)
      return definition->sensitivity;
   return unknown_rel_sensitivity(normalized[0] ? normalized : rel_type);
}

static db2_memory_pii_sensitivity_batch_fn sensitivity_batch;

void memory_pii_register_sensitivity_batch(db2_memory_pii_sensitivity_batch_fn classifier)
{
   sensitivity_batch = classifier;
}

int memory_pii_rel_sensitivity_batch(const char *const *rel_types, int count, int *out)
{
   if (!rel_types || !out || count <= 0)
      return -1;
   if (sensitivity_batch)
      return sensitivity_batch(rel_types, count, out) == 0 ? 0 : -1;
   for (int i = 0; i < count; i++)
      out[i] = memory_pii_rel_sensitivity(rel_types[i]);
   return 0;
}
