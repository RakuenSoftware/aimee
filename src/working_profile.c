/* working_profile.c: learned user-preference layer backed by the DB1
 * working_profile_*_local subsystem. Observations accumulate until a
 * value has repeated enough to clear the commit threshold, at which
 * point that value becomes the committed entry for the field. Pair
 * with the operator-authored charter in `legacy_config_record`. */

#include "aimee.h"
#include "working_profile.h"
#include "db1.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CANONICAL_FIELDS[] = {WORKING_PROFILE_FIELD_COMMUNICATION_STYLE,
                                         WORKING_PROFILE_FIELD_VERBOSITY,
                                         WORKING_PROFILE_FIELD_WORKING_HABITS,
                                         WORKING_PROFILE_FIELD_TRUST_CALIBRATION,
                                         WORKING_PROFILE_FIELD_RELATIONSHIP_NOTES,
                                         WORKING_PROFILE_FIELD_PROJECT_ROLE,
                                         NULL};

int working_profile_field_is_canonical(const char *field)
{
   if (!field || !field[0])
      return 0;
   for (int i = 0; CANONICAL_FIELDS[i]; i++)
      if (strcmp(CANONICAL_FIELDS[i], field) == 0)
         return 1;
   return 0;
}

/* --- working_profile_autoobserve_from_feedback ------------------------------
 *
 * A deliberately narrow phrase-to-observation table. Everything here
 * is lower-confidence (capped at 0.7) so the commit-threshold logic
 * in working_profile_observe keeps a single noisy turn from flipping
 * a strong existing commit. Unknown phrases are simply ignored — the
 * heuristic is opt-in, and missing a match is never wrong. */

typedef struct
{
   const char *needle;
   const char *field;
   const char *value;
   double confidence;
} working_profile_autoobserve_rule_t;

/* Rules are evaluated in order; the first matching needle for any
 * given (field, value) pair is the one that fires, and every matching
 * rule records an observation so phrases that legitimately cover
 * multiple fields ("be terse and direct") count both signals. */
static const working_profile_autoobserve_rule_t AUTOOBSERVE_RULES[] = {
    {"prefer terse", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.7},
    {"be more terse", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.7},
    {"be terse", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.7},
    {"more concise", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.65},
    {"be concise", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.7},
    {"keep it short", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.6},
    {"be brief", WORKING_PROFILE_FIELD_VERBOSITY, "terse", 0.7},
    {"prefer verbose", WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.7},
    {"more detail", WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.6},
    {"explain more", WORKING_PROFILE_FIELD_VERBOSITY, "verbose", 0.65},
    {"be direct", WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct", 0.7},
    {"just answer", WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct", 0.65},
    {"stop apologizing", WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "direct", 0.7},
    {"be more cautious", WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "cautious", 0.7},
    {"think through it", WORKING_PROFILE_FIELD_COMMUNICATION_STYLE, "deliberate", 0.65},
    {"ask first", WORKING_PROFILE_FIELD_TRUST_CALIBRATION, "ask_before_acting", 0.7},
    {"don't guess", WORKING_PROFILE_FIELD_TRUST_CALIBRATION, "ask_before_acting", 0.7},
    {NULL, NULL, NULL, 0.0},
};

int working_profile_autoobserve_from_feedback(const char *text)
{
   if (!text || !text[0])
      return 0;

   /* Lowercase the input once so substring matches are case-
    * insensitive. Bounded length mirrors the feedback surface: the
    * UI typically caps feedback at ~2 KiB. */
   size_t tlen = strlen(text);
   if (tlen > 4096)
      tlen = 4096;
   char lowered[4097];
   for (size_t i = 0; i < tlen; i++)
      lowered[i] = (char)tolower((unsigned char)text[i]);
   lowered[tlen] = '\0';

   int observed = 0;
   for (int i = 0; AUTOOBSERVE_RULES[i].needle; i++)
   {
      const working_profile_autoobserve_rule_t *rule = &AUTOOBSERVE_RULES[i];
      if (!strstr(lowered, rule->needle))
         continue;
      if (db1_working_profile_local_observe(rule->field, rule->value, rule->confidence,
                                            session_id(), 0) >= 0)
         observed++;
   }
   return observed;
}
