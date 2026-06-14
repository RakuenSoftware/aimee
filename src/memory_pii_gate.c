/* memory_pii_gate.c: per-attribute PII recall gating (§7). Pure. P5.
 * See memory_pii_gate.h. */
#include "headers/memory_pii_gate.h"
#include "headers/rel_types.h" /* rel_types_seed_lookup, rel_type_normalize */

#include <ctype.h>
#include <string.h>

/* Case-insensitive substring search. */
static int ci_contains(const char *hay, const char *needle)
{
   size_t hn = strlen(hay), nn = strlen(needle);
   if (nn == 0)
      return 1;
   if (nn > hn)
      return 0;
   for (size_t i = 0; i + nn <= hn; i++)
   {
      size_t k = 0;
      while (k < nn && tolower((unsigned char)hay[i + k]) == tolower((unsigned char)needle[k]))
         k++;
      if (k == nn)
         return 1;
   }
   return 0;
}

int memory_pii_turn_requests_sensitive(const char *turn_text)
{
   if (!turn_text || !turn_text[0])
      return 0;
   /* Cues that the user is explicitly asking for a sensitive attribute. Kept
    * specific enough to avoid incidental matches. */
   static const char *cues[] = {"address",    "phone",           "email",           "birthday",
                                "birth date", "date of birth",   "born on",         "password",
                                "passphrase", "credential",      "secret",          "api key",
                                "ssn",        "social security", "where do i live", "where i live",
                                "my number",  "home ip",         "ip address"};
   for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); i++)
      if (ci_contains(turn_text, cues[i]))
         return 1;
   return 0;
}

rel_sensitivity_t memory_pii_rel_sensitivity(const char *rel_type)
{
   if (!rel_type || !rel_type[0])
      return SENS_PII; /* fail closed */
   char norm[REL_TYPE_NAME_MAX];
   rel_type_normalize(rel_type, norm, sizeof(norm));
   const rel_type_def_t *def = norm[0] ? rel_types_seed_lookup(norm) : NULL;
   return def ? def->sensitivity : SENS_PII; /* unknown -> fail closed */
}

int memory_pii_should_inject(rel_sensitivity_t sens, double confidence, int turn_requests_sensitive)
{
   /* Fail closed on a below-floor OR non-finite confidence: `confidence != confidence`
    * is true only for NaN, which would otherwise slip past the `<` comparison. */
   if (!(confidence >= PII_GATE_CONFIDENCE_FLOOR))
      return 0;
   switch (sens)
   {
   case SENS_NORMAL:
      return 1; /* identity/operational facts always pass above the floor */
   case SENS_PII:
      return turn_requests_sensitive ? 1 : 0; /* withheld unless explicitly asked */
   case SENS_SECRET:
   default:
      return 0; /* credentials never go through pre-injection */
   }
}
