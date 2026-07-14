/* wfe_manager_artifacts.c -- validators for the primary-as-manager artifact
 * schemas. See wfe_manager_artifacts.h for the design rationale. Pure cJSON, no
 * engine deps, so the client/server/kb all link it and it is unit-testable in
 * isolation. */
#include "wfe_manager_artifacts.h"

#include <stdio.h>
#include <string.h>

/* ---- small typed-field helpers (require + type-check; tolerate extras) ---- */

static int req_version(const cJSON *rec, int want, char *err, size_t errlen)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(rec, "schema_version");
   if (!v || !cJSON_IsNumber(v))
   {
      snprintf(err, errlen, "missing/invalid schema_version (want %d)", want);
      return -1;
   }
   if (v->valueint != want)
   {
      snprintf(err, errlen, "unsupported schema_version %d (want %d)", v->valueint, want);
      return -1;
   }
   return 0;
}

/* a non-empty string field */
static int req_str(const cJSON *rec, const char *key, char *err, size_t errlen)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(rec, key);
   if (!it || !cJSON_IsString(it) || !it->valuestring || !it->valuestring[0])
   {
      snprintf(err, errlen, "missing/empty string field '%s'", key);
      return -1;
   }
   return 0;
}

/* an array field (possibly empty unless min>0); returns the array or NULL+err */
static const cJSON *req_arr(const cJSON *rec, const char *key, int min, char *err, size_t errlen)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(rec, key);
   if (!it || !cJSON_IsArray(it))
   {
      snprintf(err, errlen, "missing/invalid array field '%s'", key);
      return NULL;
   }
   if (cJSON_GetArraySize(it) < min)
   {
      snprintf(err, errlen, "array field '%s' requires >= %d entries", key, min);
      return NULL;
   }
   return it;
}

/* every element of `arr` must be a non-empty string */
static int all_strings(const cJSON *arr, const char *what, char *err, size_t errlen)
{
   const cJSON *it = NULL;
   cJSON_ArrayForEach(it, arr)
   {
      if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0])
      {
         snprintf(err, errlen, "%s must be non-empty strings", what);
         return -1;
      }
   }
   return 0;
}

/* ---- intent record (understand) ---- */

/* case-insensitive substring (portable strcasestr) */
static int icontains(const char *hay, const char *needle)
{
   size_t nl = strlen(needle);
   for (const char *p = hay; *p; p++)
   {
      size_t i = 0;
      while (i < nl && p[i] &&
             ((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]) ==
                 ((needle[i] >= 'A' && needle[i] <= 'Z') ? needle[i] + 32 : needle[i]))
         i++;
      if (i == nl)
         return 1;
   }
   return 0;
}

/* Reject a SELF-REFERENTIAL intent: a record whose text is about the record /
 * work-item bookkeeping instead of the engineering task. Observed live (3 of 13
 * slices): scope delegates pattern-match the instruction into "the task is to
 * create an intent record" — the schema is valid, so the garbage advanced and
 * implement dutifully committed only .wfe-scope.json, which the roundtable then
 * rejects every round to its cap. Markers: mentions of the record itself, of
 * "work item" (a packet describes code, not run bookkeeping), a work-item id
 * (wi_<hex>), or any >=16-hex-digit run id blob. */
static int intent_self_referential(const char *s)
{
   if (icontains(s, "intent record") || icontains(s, "work item") || strstr(s, "wi_"))
      return 1;
   int hexrun = 0;
   for (const char *p = s; *p; p++)
   {
      int ishex = (*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F');
      hexrun = ishex ? hexrun + 1 : 0;
      if (hexrun >= 16)
         return 1;
   }
   return 0;
}

int wfe_intent_validate(const cJSON *rec, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!rec || !cJSON_IsObject(rec))
   {
      snprintf(err, errlen, "intent: not an object");
      return -1;
   }
   if (req_version(rec, WFE_INTENT_SCHEMA_VERSION, err, errlen) != 0)
      return -1;
   /* status: required, enum */
   const cJSON *st = cJSON_GetObjectItemCaseSensitive(rec, "status");
   if (!st || !cJSON_IsString(st) ||
       (strcmp(st->valuestring, "unconfirmed") != 0 && strcmp(st->valuestring, "confirmed") != 0))
   {
      snprintf(err, errlen, "intent: status must be 'unconfirmed' or 'confirmed'");
      return -1;
   }
   if (req_str(rec, "summary", err, errlen) != 0)
      return -1;
   const cJSON *ac = req_arr(rec, "acceptance_criteria", 1, err, errlen);
   if (!ac || all_strings(ac, "intent.acceptance_criteria", err, errlen) != 0)
      return -1;
   /* Content gate, not just shape: a self-referential record loops the scope
    * attempt (fresh delegate) instead of poisoning every downstream stage. */
   const cJSON *sm = cJSON_GetObjectItemCaseSensitive(rec, "summary");
   if (intent_self_referential(sm->valuestring))
   {
      snprintf(err, errlen,
               "intent: self-referential — summary must scope the engineering task itself, not "
               "the intent record / work item bookkeeping");
      return -1;
   }
   const cJSON *it = NULL;
   cJSON_ArrayForEach(it, ac)
   {
      if (icontains(it->valuestring, "intent record"))
      {
         snprintf(err, errlen,
                  "intent: self-referential acceptance criterion — criteria must be "
                  "testable properties of the change, not of the record");
         return -1;
      }
   }
   return 0;
}

int wfe_intent_status(const cJSON *rec, wfe_intent_status_t *out)
{
   char err[8];
   if (!out || wfe_intent_validate(rec, err, sizeof err) != 0)
      return -1;
   const cJSON *st = cJSON_GetObjectItemCaseSensitive(rec, "status");
   *out = strcmp(st->valuestring, "confirmed") == 0 ? WFE_INTENT_CONFIRMED : WFE_INTENT_UNCONFIRMED;
   return 0;
}

/* ---- packet plan (split) ---- */

int wfe_packets_validate(const cJSON *rec, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!rec || !cJSON_IsObject(rec))
   {
      snprintf(err, errlen, "packets: not an object");
      return -1;
   }
   if (req_version(rec, WFE_PACKETS_SCHEMA_VERSION, err, errlen) != 0)
      return -1;
   const cJSON *packets = req_arr(rec, "packets", 1, err, errlen);
   if (!packets)
      return -1;
   const cJSON *p = NULL;
   int idx = 0;
   cJSON_ArrayForEach(p, packets)
   {
      if (!cJSON_IsObject(p))
      {
         snprintf(err, errlen, "packets[%d]: not an object", idx);
         return -1;
      }
      if (req_str(p, "packet_id", err, errlen) != 0 || req_str(p, "summary", err, errlen) != 0)
      {
         /* prepend packet index for locality */
         char inner[200];
         snprintf(inner, sizeof inner, "packets[%d]: %s", idx, err);
         snprintf(err, errlen, "%s", inner);
         return -1;
      }
      /* target_blocks + acceptance_criteria required non-empty; dependencies may
       * be empty but must be present + string-typed. */
      const cJSON *tb = req_arr(p, "target_blocks", 1, err, errlen);
      if (!tb || all_strings(tb, "target_blocks", err, errlen) != 0)
         return -1;
      const cJSON *ac = req_arr(p, "acceptance_criteria", 1, err, errlen);
      if (!ac || all_strings(ac, "acceptance_criteria", err, errlen) != 0)
         return -1;
      const cJSON *dep = req_arr(p, "dependencies", 0, err, errlen);
      if (!dep || all_strings(dep, "dependencies", err, errlen) != 0)
         return -1;
      idx++;
   }
   return 0;
}

/* ---- review verdict (review) ---- */

int wfe_review_validate(const cJSON *rec, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!rec || !cJSON_IsObject(rec))
   {
      snprintf(err, errlen, "review: not an object");
      return -1;
   }
   if (req_version(rec, WFE_REVIEW_SCHEMA_VERSION, err, errlen) != 0)
      return -1;
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(rec, "verdict");
   int is_pass = v && cJSON_IsString(v) && strcmp(v->valuestring, "pass") == 0;
   int is_changes = v && cJSON_IsString(v) && strcmp(v->valuestring, "changes") == 0;
   if (!is_pass && !is_changes)
   {
      snprintf(err, errlen, "review: verdict must be 'pass' or 'changes'");
      return -1;
   }
   /* blocking_findings + non_blocking are required arrays; a 'changes' verdict
    * MUST carry >= 1 blocking finding (an empty-changes verdict is meaningless
    * and would loop forever with no delta for the re-delegated engineer). */
   const cJSON *bf = req_arr(rec, "blocking_findings", is_changes ? 1 : 0, err, errlen);
   if (!bf)
      return -1;
   if (!req_arr(rec, "non_blocking", 0, err, errlen))
      return -1;
   /* each blocking finding must carry the full, falsifiable shape so the
    * re-delegated engineer (and the following roundtable) has a machine-checkable
    * contract, not prose. */
   const cJSON *f = NULL;
   int idx = 0;
   cJSON_ArrayForEach(f, bf)
   {
      if (!cJSON_IsObject(f))
      {
         snprintf(err, errlen, "blocking_findings[%d]: not an object", idx);
         return -1;
      }
      static const char *fields[] = {"block_id", "rule_id", "expected", "observed",
                                     "suggested_fix"};
      for (size_t k = 0; k < sizeof fields / sizeof fields[0]; k++)
         if (req_str(f, fields[k], err, errlen) != 0)
         {
            char inner[200];
            snprintf(inner, sizeof inner, "blocking_findings[%d]: %s", idx, err);
            snprintf(err, errlen, "%s", inner);
            return -1;
         }
      idx++;
   }
   return 0;
}

int wfe_review_verdict(const cJSON *rec, wfe_review_verdict_t *out)
{
   char err[8];
   if (!out || wfe_review_validate(rec, err, sizeof err) != 0)
      return -1;
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(rec, "verdict");
   *out = strcmp(v->valuestring, "pass") == 0 ? WFE_REVIEW_PASS : WFE_REVIEW_CHANGES;
   return 0;
}
