/* roundtable_chair.c: the reasoning-refutation pass. See roundtable_chair.h. */

#include "aimee.h" /* MAX_PATH_LEN (pulled by delegate_ensemble.h -> agent_types.h) */

#include "roundtable_chair.h"

#include "cJSON.h"
#include "dstr.h"

#include <stdlib.h>
#include <string.h>

/* sev_rank: lower number = less severe (nit=1 < suggestion=2 < blocking=3); the demote
 * guard requires a STRICT decrease, so it can only ever lower severity. */
static int sev_rank(const char *s)
{
   if (!s)
      return 0;
   if (strcmp(s, "blocking") == 0)
      return 3;
   if (strcmp(s, "suggestion") == 0)
      return 2;
   if (strcmp(s, "nit") == 0)
      return 1;
   return 0;
}

/* A demote may only target a lower, KNOWN severity — never "blocking" (an escalation)
 * and never an unknown string. */
static int valid_demote_target(const char *s)
{
   return s && (strcmp(s, "suggestion") == 0 || strcmp(s, "nit") == 0);
}

char *roundtable_chair_build_prompt(const roundtable_result_t *out)
{
   if (!out || out->item_count <= 0)
      return NULL;

   dstr_t s;
   dstr_init(&s);
   dstr_append_str(
       &s,
       "You are the CHAIR of a code-review roundtable. The findings below already SURVIVED "
       "evidence-verification against the code index, so each is grounded in real code. Your job "
       "is judgment the index cannot make: is a finding genuinely actionable, or is it "
       "technically true yet OVER-FLAGGED — low impact, or already defended elsewhere?\n\n"
       "You may do exactly two things, and ONLY with a written rationale:\n"
       "  - DEMOTE an over-severe finding (e.g. a blocking issue that is really a suggestion).\n"
       "  - DROP an over-flagged finding (technically true but not worth acting on).\n"
       "RULES: DEFAULT TO KEEP — only demote/drop when you can clearly justify it. You CANNOT "
       "escalate a severity and CANNOT add new findings; anything else is ignored. Refute like a "
       "reviewer defending the code, citing the specific reason (a guard elsewhere, the true "
       "blast radius, an accepted trade-off).\n\n"
       "FINDINGS:\n");
   for (int i = 0; i < out->item_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
   {
      const roundtable_review_item_t *it = &out->items[i];
      dstr_appendf(&s, "%d. [%s] (%s) %s\n", i + 1, it->severity[0] ? it->severity : "suggestion",
                   it->location[0] ? it->location : "?", it->summary);
   }
   dstr_append_str(
       &s,
       "\nReturn ONLY raw JSON (no prose, no code fences):\n"
       "{\"verdicts\":[{\"summary\":\"<the finding's summary text, verbatim>\",\"verdict\":\"keep|"
       "demote|drop\",\"new_severity\":\"suggestion|nit\",\"rationale\":\"why, in one line "
       "(required for demote/drop)\"}]}\n"
       "Include only the findings you demote or drop; omit the ones you keep.");
   return dstr_steal(&s);
}

/* Skip to the first '{' so a fenced/prefixed LLM reply still parses. */
static cJSON *parse_verdicts(const char *text)
{
   if (!text)
      return NULL;
   const char *p = strchr(text, '{');
   if (!p)
      return NULL;
   return cJSON_Parse(p);
}

char *roundtable_chair_apply(roundtable_result_t *out, const char *chair_json, int *changed)
{
   if (changed)
      *changed = 0;
   if (!out || out->item_count <= 0)
      return NULL;

   cJSON *doc = parse_verdicts(chair_json);
   if (!doc)
      return NULL; /* unparseable -> chair no-op, out untouched */
   cJSON *verdicts = cJSON_GetObjectItemCaseSensitive(doc, "verdicts");
   if (!cJSON_IsArray(verdicts))
   {
      cJSON_Delete(doc);
      return NULL;
   }

   dstr_t md;
   dstr_init(&md);
   dstr_append_str(&md, "\n## Chair adjudication\n");
   int n_changed = 0;

   /* O(items * verdicts) with a strcmp per pair — bounded by ROUNDTABLE_MAX_REVIEW_ITEMS
    * on both sides, so trivial at current caps; revisit (hash) only if the caps grow. */
   int keep = 0;
   for (int i = 0; i < out->item_count; i++)
   {
      roundtable_review_item_t it = out->items[i];

      /* Find this item's verdict by exact summary match (the chair was given the
       * verbatim summaries). An empty summary is unmatchable. No match -> keep
       * untouched (conservative default). */
      const char *verdict = NULL, *new_sev = NULL, *rationale = NULL;
      if (it.summary[0])
      {
         cJSON *v;
         cJSON_ArrayForEach(v, verdicts)
         {
            cJSON *sum = cJSON_GetObjectItemCaseSensitive(v, "summary");
            if (cJSON_IsString(sum) && sum->valuestring[0] &&
                strcmp(sum->valuestring, it.summary) == 0)
            {
               cJSON *vd = cJSON_GetObjectItemCaseSensitive(v, "verdict");
               cJSON *ns = cJSON_GetObjectItemCaseSensitive(v, "new_severity");
               cJSON *ra = cJSON_GetObjectItemCaseSensitive(v, "rationale");
               verdict = cJSON_IsString(vd) ? vd->valuestring : NULL;
               new_sev = cJSON_IsString(ns) ? ns->valuestring : NULL;
               if (cJSON_IsString(ra) && ra->valuestring[0])
                  rationale = ra->valuestring;
               break;
            }
         }
      }

      /* A demote/drop is only honored WITH a rationale (the contract) — otherwise the
       * verdict is ignored and the survivor is kept unchanged. */
      if (verdict && rationale && strcmp(verdict, "drop") == 0)
      {
         /* Transparent drop: ONLY if it can be recorded in the rejected list. If that
          * list is full, keep the survivor rather than silently lose it. */
         if (out->rejected_count < ROUNDTABLE_MAX_REVIEW_ITEMS)
         {
            out->rejected[out->rejected_count] = it;
            snprintf(out->rejected_reason[out->rejected_count],
                     sizeof(out->rejected_reason[out->rejected_count]), "chair-refuted");
            out->rejected_count++;
            dstr_appendf(&md, "- DROP **%s** — %s\n", it.summary, rationale);
            n_changed++;
            continue; /* removed from items[] */
         }
         /* rejected full -> fall through and keep the item unchanged */
      }
      else if (verdict && rationale && strcmp(verdict, "demote") == 0 &&
               valid_demote_target(new_sev) && sev_rank(new_sev) < sev_rank(it.severity))
      {
         dstr_appendf(&md, "- DEMOTE **%s** %s→%s — %s\n", it.summary, it.severity, new_sev,
                      rationale);
         snprintf(it.severity, sizeof(it.severity), "%s", new_sev);
         n_changed++;
      }

      out->items[keep++] = it;
   }
   out->item_count = keep;

   cJSON_Delete(doc);

   if (n_changed == 0)
   {
      dstr_free(&md);
      if (changed)
         *changed = 0;
      return NULL;
   }
   if (changed)
      *changed = n_changed;
   return dstr_steal(&md);
}
