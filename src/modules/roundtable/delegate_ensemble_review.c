/* delegate_ensemble_review.c: split from delegate_ensemble.c into a real translation unit
 * (was delegate_ensemble_review.inc, textually included only to stay under the
 * line-check ceiling). Cross-TU declarations live in the module header. */
#include "delegate_ensemble_internal.h"
#include "aimee.h"
#include "delegate_ensemble.h"
#include "agent_exec.h"
#include "agent_config.h"
#include "config.h"
#include "cost_fold.h"
#include "log.h"
#include "persona.h"
#include "dstr.h"
#include "roundtable_verify.h"
#include "token_tracker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

static void normalized_identity_key(const char *category, const char *location, const char *summary,
                                    char *out, size_t out_n)
{
   if (!out || out_n == 0)
      return;
   const char *parts[2] = {(category && category[0]) ? category : "general",
                           (location && location[0]) ? location : (summary ? summary : "")};
   size_t pos = 0;
   for (int part = 0; part < 2; part++)
   {
      const char *s = parts[part];
      while (*s && pos + 1 < out_n)
      {
         if (isalnum((unsigned char)*s))
            out[pos++] = (char)tolower((unsigned char)*s);
         else if (pos > 0 && out[pos - 1] != ':')
            out[pos++] = ':';
         s++;
      }
      if (part == 0 && pos > 0 && pos + 1 < out_n && out[pos - 1] != ':')
         out[pos++] = ':';
   }
   while (pos > 0 && out[pos - 1] == ':')
      pos--;
   out[pos] = '\0';
}

int parse_review_issue_keys(const char *text, char keys[][128], int *count, int max)
{
   if (!text || !count || max <= 0)
      return -1;
   cJSON *root = parse_model_json_lenient(text);
   if (!root)
      return -1;
   cJSON *issues = cJSON_GetObjectItemCaseSensitive(root, "issues");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "items");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "blocking_issues");
   if (!cJSON_IsArray(issues))
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *it;
   cJSON_ArrayForEach(it, issues)
   {
      cJSON *severity = cJSON_GetObjectItemCaseSensitive(it, "severity");
      cJSON *category = cJSON_GetObjectItemCaseSensitive(it, "category");
      cJSON *location = cJSON_GetObjectItemCaseSensitive(it, "location");
      cJSON *summary = cJSON_GetObjectItemCaseSensitive(it, "summary");
      if (cJSON_IsString(severity) && strcmp(severity->valuestring, "blocking") != 0)
         continue;
      char key[128];
      normalized_identity_key(cJSON_IsString(category) ? category->valuestring : "",
                              cJSON_IsString(location) ? location->valuestring : "",
                              cJSON_IsString(summary) ? summary->valuestring : "", key,
                              sizeof(key));
      if (!key[0] || key_seen128(keys, *count, key))
         continue;
      if (*count < max)
         snprintf(keys[(*count)++], 128, "%s", key);
   }
   cJSON_Delete(root);
   return 0;
}

static int review_items_same(const roundtable_review_item_t *a, const char *identity,
                             const char *summary)
{
   return a && identity && summary && strcmp(a->identity_key, identity) == 0 &&
          strcmp(a->summary, summary) == 0;
}

/* Seat label for attribution: the agent AND the model it actually served.
 *
 * A registered agent is not always one model — a provider-general registration
 * (codex -> sol/terra/luna) makes the bare agent name ambiguous, and the models
 * within a provider differ in capability, which is precisely what a reviewer
 * needs to know when weighing a finding. Uses the canonical `agent:model` form
 * already understood elsewhere (model_capability_resolve_ref), kept compact
 * because `sources` is a fixed 256-byte comma-joined list shared by every seat.
 *
 * Falls back to the bare name when the model is unknown or merely repeats it —
 * several agents are named after their single model today, and "MiniMax-M3:
 * MiniMax-M3" would be noise. */
static void review_seat_label(const agent_result_t *r, char *buf, size_t len)
{
   if (!buf || len == 0)
      return;
   buf[0] = '\0';
   if (!r)
      return;
   const char *name = r->agent_name;
   const char *model = r->served_model[0] ? r->served_model : r->model;
   if (!name || !name[0])
      name = model && model[0] ? model : "";
   if (!model || !model[0] || strcmp(model, name) == 0)
   {
      snprintf(buf, len, "%s", name);
      return;
   }
   snprintf(buf, len, "%s:%s", name, model);
}

static void review_item_add_source(roundtable_review_item_t *item, const char *source)
{
   if (!item)
      return;
   item->count++;
   if (!source || !source[0])
      return;
   if (strstr(item->sources, source))
      return;
   size_t used = strlen(item->sources);
   if (used > 0 && used + 2 < sizeof(item->sources))
   {
      item->sources[used++] = ',';
      item->sources[used++] = ' ';
      item->sources[used] = '\0';
   }
   if (used + 1 < sizeof(item->sources))
      snprintf(item->sources + used, sizeof(item->sources) - used, "%s", source);
}

static cJSON *review_items_array(cJSON *root)
{
   cJSON *issues = cJSON_GetObjectItemCaseSensitive(root, "issues");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "items");
   if (!cJSON_IsArray(issues))
      issues = cJSON_GetObjectItemCaseSensitive(root, "blocking_issues");
   return cJSON_IsArray(issues) ? issues : NULL;
}

static int alignment_rank(const char *status)
{
   if (status && strcmp(status, "drifted") == 0)
      return 3;
   if (status && strcmp(status, "unclear") == 0)
      return 2;
   if (status && strcmp(status, "aligned") == 0)
      return 1;
   return 0;
}

static void capture_original_request_alignment(cJSON *root, const char *source,
                                               roundtable_result_t *out)
{
   cJSON *a = cJSON_GetObjectItemCaseSensitive(root, "original_request_alignment");
   cJSON *status = cJSON_IsObject(a) ? cJSON_GetObjectItemCaseSensitive(a, "status") : NULL;
   cJSON *summary = cJSON_IsObject(a) ? cJSON_GetObjectItemCaseSensitive(a, "summary") : NULL;
   if (!cJSON_IsString(status) || alignment_rank(status->valuestring) == 0)
      return;
   int incoming = alignment_rank(status->valuestring);
   int current = alignment_rank(out->original_request_alignment);
   if (incoming > current || (incoming == current && !out->original_request_alignment_summary[0]))
   {
      snprintf(out->original_request_alignment, sizeof out->original_request_alignment, "%s",
               status->valuestring);
      snprintf(out->original_request_alignment_summary,
               sizeof out->original_request_alignment_summary, "%s",
               cJSON_IsString(summary) && summary->valuestring[0]
                   ? summary->valuestring
                   : "The reviewer returned no explanation for its alignment verdict.");
      out->original_request_alignment_sources[0] = '\0';
   }
   if (incoming == alignment_rank(out->original_request_alignment) && source && source[0] &&
       !strstr(out->original_request_alignment_sources, source))
   {
      size_t used = strlen(out->original_request_alignment_sources);
      snprintf(out->original_request_alignment_sources + used,
               sizeof out->original_request_alignment_sources - used, "%s%s", used ? ", " : "",
               source);
   }
}

/* Parse the optional "evidence" object a panelist attaches to an item into a
 * structured review_evidence_t for the replay verifier. Absent/unknown -> EV_NONE
 * (interpretive: the verifier caps it below blocking). */
static void parse_review_evidence(const cJSON *item, review_evidence_t *ev)
{
   memset(ev, 0, sizeof(*ev));
   const cJSON *e = cJSON_GetObjectItemCaseSensitive(item, "evidence");
   if (!cJSON_IsObject(e))
      return;
   const cJSON *kind = cJSON_GetObjectItemCaseSensitive(e, "kind");
   const char *k = cJSON_IsString(kind) ? kind->valuestring : "";
   if (strcmp(k, "refs") == 0)
      ev->kind = EV_REFS;
   else if (strcmp(k, "symbol") == 0)
      ev->kind = EV_SYMBOL;
   else if (strcmp(k, "search") == 0)
      ev->kind = EV_SEARCH;
   else
      return; /* "none"/unknown -> EV_NONE (already zeroed) */
   const cJSON *target = cJSON_GetObjectItemCaseSensitive(e, "target");
   const cJSON *project = cJSON_GetObjectItemCaseSensitive(e, "project");
   const cJSON *count = cJSON_GetObjectItemCaseSensitive(e, "count");
   const cJSON *factual = cJSON_GetObjectItemCaseSensitive(e, "factual");
   if (cJSON_IsString(target))
      snprintf(ev->target, sizeof(ev->target), "%s", target->valuestring);
   if (cJSON_IsString(project))
      snprintf(ev->project, sizeof(ev->project), "%s", project->valuestring);
   if (cJSON_IsNumber(count) && count->valueint > 0)
      ev->count = count->valueint;
   /* factual is true only if explicitly set true AND a target was given */
   ev->factual = cJSON_IsBool(factual) ? cJSON_IsTrue(factual) : 0;
   if (!ev->target[0])
      ev->kind = EV_NONE; /* a kind with no target is unusable -> interpretive */
}

static void capture_review_items_from_text(const char *text, const char *source, int tool_grounded,
                                           roundtable_result_t *out)
{
   if (!text || !out)
      return;
   cJSON *root = parse_model_json_lenient(text);
   if (!root)
      return;
   capture_original_request_alignment(root, source, out);
   cJSON *issues = review_items_array(root);
   if (!issues)
   {
      cJSON_Delete(root);
      return;
   }
   cJSON *it;
   cJSON_ArrayForEach(it, issues)
   {
      cJSON *severity = cJSON_GetObjectItemCaseSensitive(it, "severity");
      cJSON *category = cJSON_GetObjectItemCaseSensitive(it, "category");
      cJSON *location = cJSON_GetObjectItemCaseSensitive(it, "location");
      cJSON *summary = cJSON_GetObjectItemCaseSensitive(it, "summary");
      cJSON *recommendation = cJSON_GetObjectItemCaseSensitive(it, "recommendation");
      const char *sev =
          cJSON_IsString(severity) && severity->valuestring[0] ? severity->valuestring : "blocking";
      const char *cat = cJSON_IsString(category) ? category->valuestring : "";
      const char *loc = cJSON_IsString(location) ? location->valuestring : "";
      const char *sum = cJSON_IsString(summary) ? summary->valuestring : "";
      const char *rec = cJSON_IsString(recommendation) ? recommendation->valuestring : "";
      char identity[128];
      normalized_identity_key(cat, loc, sum, identity, sizeof(identity));
      if (!identity[0] || !sum[0])
         continue;
      int idx = -1;
      for (int i = 0; i < out->item_count; i++)
      {
         if (review_items_same(&out->items[i], identity, sum))
         {
            idx = i;
            break;
         }
      }
      if (idx < 0)
      {
         if (out->item_count >= ROUNDTABLE_MAX_REVIEW_ITEMS)
         {
            out->truncated = 1;
            continue;
         }
         idx = out->item_count++;
         memset(&out->items[idx], 0, sizeof(out->items[idx]));
         snprintf(out->items[idx].severity, sizeof(out->items[idx].severity), "%s", sev);
         snprintf(out->items[idx].category, sizeof(out->items[idx].category), "%s", cat);
         snprintf(out->items[idx].location, sizeof(out->items[idx].location), "%s", loc);
         snprintf(out->items[idx].summary, sizeof(out->items[idx].summary), "%s", sum);
         snprintf(out->items[idx].recommendation, sizeof(out->items[idx].recommendation), "%s",
                  rec);
         snprintf(out->items[idx].identity_key, sizeof(out->items[idx].identity_key), "%s",
                  identity);
         out->items[idx].tool_grounded = tool_grounded ? 1 : 0;
         parse_review_evidence(it, &out->items[idx].evidence);
      }
      else if (tool_grounded)
         out->items[idx].tool_grounded = 1;
      review_item_add_source(&out->items[idx], source);
   }
   cJSON_Delete(root);
}

void capture_round_review_items(const agent_result_t *results, int ref_count,
                                roundtable_result_t *out, int round)
{
   if (!results || !out)
      return;
   out->item_count = 0;
   out->items_round = round;
   memset(out->items, 0, sizeof(out->items));
   out->original_request_alignment[0] = '\0';
   out->original_request_alignment_summary[0] = '\0';
   out->original_request_alignment_sources[0] = '\0';
   for (int i = 0; i < ref_count; i++)
   {
      char seat[MAX_AGENT_NAME + MAX_MODEL_LEN + 2];
      review_seat_label(&results[i], seat, sizeof(seat));
      capture_review_items_from_text(results[i].response, seat,
                                     results[i].successful_tool_calls > 0, out);
   }
   if (!out->original_request_alignment[0])
   {
      snprintf(out->original_request_alignment, sizeof out->original_request_alignment, "unclear");
      snprintf(out->original_request_alignment_summary,
               sizeof out->original_request_alignment_summary,
               "No panelist returned the required original-request alignment assessment.");
   }
}
