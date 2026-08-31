/* learning_eval_synthesis.c: the pure half of failure -> regression task.
 *
 * See docs/proposals/pending/recursive-self-improvement-closing-the-loops.md */

#include <aimee/learning/eval_synthesis.h>

#include "cJSON.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Markup characters that carry prompt-injection structure. Rejected outright
 * rather than escaped — see the header for why this is not a sanitizer. */
static const char *const LEARNING_EVAL_FORBIDDEN = "<>[]|#\\`${}";

int learning_eval_text_admissible(const char *s)
{
   if (!s)
      return 1; /* absent */
   size_t n = strlen(s);
   if (n > LEARNING_EVAL_MAX_FIELD)
      return 0;
   for (size_t i = 0; i < n; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (c < 0x20 || c == 0x7f)
         return 0; /* control characters, newline and tab included */
      if (c >= 0x80)
         return 0; /* keep stored task text to plain ASCII */
      if (strchr(LEARNING_EVAL_FORBIDDEN, (char)c))
         return 0;
   }
   return 1;
}

int learning_eval_failure_admissible(const learning_eval_failure_t *f)
{
   if (!f)
      return 0;
   return learning_eval_text_admissible(f->origin) &&
          learning_eval_text_admissible(f->origin_ref) && learning_eval_text_admissible(f->role) &&
          learning_eval_text_admissible(f->prompt) &&
          learning_eval_text_admissible(f->failure_mode) &&
          learning_eval_text_admissible(f->check_type) &&
          learning_eval_text_admissible(f->check_value);
}

/* Case-fold, collapse every run of non-alphanumerics to one space, trim. */
static void normalize_into(const char *src, char *buf, size_t cap)
{
   size_t o = 0;
   int pending_space = 0;
   for (const char *p = src ? src : ""; *p && o + 1 < cap; p++)
   {
      unsigned char c = (unsigned char)*p;
      if (isalnum(c))
      {
         if (pending_space && o > 0 && o + 1 < cap)
            buf[o++] = ' ';
         pending_space = 0;
         if (o + 1 < cap)
            buf[o++] = (char)tolower(c);
      }
      else if (o > 0)
      {
         pending_space = 1;
      }
   }
   buf[o] = '\0';
}

/* FNV-1a over a normalised field stream, seeded so two passes give
 * independent halves of the signature. */
static uint64_t fnv1a_seeded(uint64_t basis, const char *const *fields, int n)
{
   uint64_t h = basis;
   for (int i = 0; i < n; i++)
   {
      char norm[LEARNING_EVAL_MAX_FIELD + 1];
      normalize_into(fields[i], norm, sizeof(norm));
      for (const char *p = norm; *p; p++)
         h = (h ^ (unsigned char)*p) * 1099511628211ULL;
      h = (h ^ 0x1f) * 1099511628211ULL; /* field separator */
   }
   return h;
}

int learning_eval_signature(const learning_eval_failure_t *f, char *out, size_t out_len)
{
   if (!f || !out || out_len < LEARNING_EVAL_SIGNATURE_LEN)
      return -1;
   if (!learning_eval_failure_admissible(f))
      return -2;

   const char *fields[4] = {
       (f->role && f->role[0]) ? f->role : "execute",
       f->failure_mode ? f->failure_mode : "",
       f->check_type ? f->check_type : "",
       f->check_value ? f->check_value : "",
   };
   uint64_t a = fnv1a_seeded(1469598103934665603ULL, fields, 4);
   uint64_t b = fnv1a_seeded(0x9e3779b97f4a7c15ULL, fields, 4);
   snprintf(out, out_len, "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
   return 0;
}

int learning_eval_task_name(const char *signature, char *out, size_t out_len)
{
   if (!signature || !signature[0] || !out || out_len < 24)
      return -1;
   for (const char *p = signature; *p; p++)
      if (!isxdigit((unsigned char)*p))
         return -1;
   snprintf(out, out_len, "regression-%.12s", signature);
   return 0;
}

int learning_eval_build_task(const learning_eval_failure_t *f, const char *task_name, char *out,
                             size_t out_len)
{
   if (!f || !task_name || !task_name[0] || !out || out_len == 0)
      return -1;
   if (!learning_eval_failure_admissible(f))
      return -2;
   /* Nothing to replay is not a task. */
   if (!f->prompt || !f->prompt[0])
      return -1;
   /* A substring check without a substring would pass on every response. */
   const int has_check = f->check_type && f->check_type[0];
   if (has_check && strcmp(f->check_type, "contains") == 0 &&
       (!f->check_value || !f->check_value[0]))
      return -1;

   cJSON *obj = cJSON_CreateObject();
   if (!obj)
      return -1;
   cJSON_AddStringToObject(obj, "name", task_name);
   cJSON_AddStringToObject(obj, "prompt", f->prompt);
   cJSON_AddStringToObject(obj, "role", (f->role && f->role[0]) ? f->role : "execute");
   if (has_check)
   {
      cJSON *check = cJSON_AddObjectToObject(obj, "success_check");
      if (check)
      {
         cJSON_AddStringToObject(check, "type", f->check_type);
         cJSON_AddStringToObject(check, "value", f->check_value ? f->check_value : "");
      }
   }
   cJSON_AddNumberToObject(obj, "max_turns", 10);
   /* Provenance travels with the task so a reader of the suite directory can
    * see this file was synthesised, and from what, without consulting the DB. */
   cJSON *prov = cJSON_AddObjectToObject(obj, "provenance");
   if (prov)
   {
      cJSON_AddBoolToObject(prov, "synthesized", 1);
      cJSON_AddStringToObject(prov, "origin", f->origin ? f->origin : "");
      cJSON_AddStringToObject(prov, "origin_ref", f->origin_ref ? f->origin_ref : "");
      cJSON_AddStringToObject(prov, "failure_mode", f->failure_mode ? f->failure_mode : "");
   }

   char *rendered = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   if (!rendered)
      return -1;
   int rc = 0;
   if (strlen(rendered) + 1 > out_len)
      rc = -1;
   else
      snprintf(out, out_len, "%s", rendered);
   free(rendered);
   return rc;
}

int learning_eval_admission_ready(int occurrences, int distinct_sessions, int min_occurrences,
                                  int gate_open)
{
   if (!gate_open)
      return 0;
   if (min_occurrences <= 0)
      min_occurrences = LEARNING_EVAL_MIN_OCCURRENCES;
   if (occurrences < min_occurrences)
      return 0;
   /* One session repeating itself is not reproduction. */
   if (distinct_sessions < min_occurrences)
      return 0;
   return 1;
}
