/* diagnose_render.c: turning a diagnosis into a document.
 *
 * These three read nothing directly. They call db1_diagnose_get, _list_items,
 * _rank_hypotheses and _suggest_probes -- operations in their own right -- and
 * format what comes back as JSON or as a status block. Formatting is not
 * storage, so it belongs on this side of the module boundary, where the caller
 * already is.
 *
 * Keeping them in the module would have meant giving the module a growable
 * string buffer and a JSON writer to serve a rendering nobody stores. The
 * queries cross the bus; the rendering does not need to.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "diagnose.h"
#include "dstr.h"

static cJSON *item_to_json(const diagnosis_item_t *it)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", it->id);
   cJSON_AddStringToObject(obj, "kind", it->kind);
   cJSON_AddNumberToObject(obj, "parent_id", it->parent_id);
   cJSON_AddStringToObject(obj, "content", it->content);
   cJSON_AddStringToObject(obj, "source", it->source);
   cJSON_AddNumberToObject(obj, "evidence_rank", it->evidence_rank);
   cJSON_AddStringToObject(obj, "created_at", it->created_at);
   return obj;
}

static cJSON *diagnosis_to_json_summary(const diagnosis_t *d)
{
   cJSON *obj = cJSON_CreateObject();
   cJSON_AddNumberToObject(obj, "id", d->id);
   cJSON_AddStringToObject(obj, "symptom", d->symptom);
   cJSON_AddStringToObject(obj, "status", d->status);
   cJSON_AddStringToObject(obj, "conclusion", d->conclusion);
   cJSON_AddNumberToObject(obj, "confidence", d->confidence);
   cJSON_AddStringToObject(obj, "created_at", d->created_at);
   cJSON_AddStringToObject(obj, "updated_at", d->updated_at);
   return obj;
}

static int clamp_rank(int rank)
{
   if (rank < DIAG_RANK_DIRECT)
      return DIAG_RANK_DIRECT;
   if (rank > DIAG_RANK_SPECULATION)
      return DIAG_RANK_SPECULATION;
   return rank;
}

static const char *rank_label(int rank)
{
   switch (clamp_rank(rank))
   {
   case DIAG_RANK_DIRECT:
      return "direct";
   case DIAG_RANK_LOG:
      return "log";
   case DIAG_RANK_CODE:
      return "code";
   default:
      return "speculation";
   }
}

char *db1_diagnose_json_full(int diag_id)
{
   diagnosis_t d;
   if (db1_diagnose_get(diag_id, &d) != 0)
      return NULL;

   cJSON *obj = diagnosis_to_json_summary(&d);

   diagnosis_item_t items[512];
   int n = db1_diagnose_list_items(diag_id, items, 512);
   cJSON *arr = cJSON_AddArrayToObject(obj, "items");
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, item_to_json(&items[i]));

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   cJSON *rarr = cJSON_AddArrayToObject(obj, "hypotheses_ranked");
   for (int i = 0; i < r; i++)
   {
      cJSON *h = cJSON_CreateObject();
      cJSON_AddNumberToObject(h, "id", rankings[i].hypothesis.id);
      cJSON_AddStringToObject(h, "content", rankings[i].hypothesis.content);
      cJSON_AddNumberToObject(h, "confidence", rankings[i].confidence);
      cJSON_AddNumberToObject(h, "evidence_for", rankings[i].evidence_for_count);
      cJSON_AddNumberToObject(h, "evidence_against", rankings[i].evidence_against_count);
      cJSON_AddNumberToObject(h, "strongest_for_rank", rankings[i].strongest_for_rank);
      cJSON_AddNumberToObject(h, "strongest_against_rank", rankings[i].strongest_against_rank);
      cJSON_AddItemToArray(rarr, h);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   return json;
}

char *db1_diagnose_json_list(void)
{
   diagnosis_t rows[128];
   int n = db1_diagnose_list(rows, 128);
   cJSON *arr = cJSON_CreateArray();
   for (int i = 0; i < n; i++)
      cJSON_AddItemToArray(arr, diagnosis_to_json_summary(&rows[i]));
   char *json = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return json;
}

char *db1_diagnose_render_status(int diag_id)
{
   diagnosis_t d;
   if (db1_diagnose_get(diag_id, &d) != 0)
      return NULL;

   dstr_t out;
   dstr_init(&out);
   dstr_appendf(&out, "# Diagnosis #%d: %s\n", d.id, d.symptom);
   dstr_appendf(&out, "Status: %s", d.status);
   if (d.confidence > 0.0 && strcmp(d.status, "concluded") == 0)
      dstr_appendf(&out, " (confidence %.2f)", d.confidence);
   dstr_append_char(&out, '\n');
   if (d.conclusion[0])
      dstr_appendf(&out, "Conclusion: %s\n", d.conclusion);

   diagnosis_item_t items[512];
   int total = db1_diagnose_list_items(diag_id, items, 512);

   dstr_append_str(&out, "\n## Observations\n");
   int obs = 0;
   for (int i = 0; i < total; i++)
      if (strcmp(items[i].kind, "observation") == 0)
      {
         obs++;
         dstr_appendf(&out, "%d. [%s] %s", obs, rank_label(items[i].evidence_rank),
                      items[i].content);
         if (items[i].source[0])
            dstr_appendf(&out, "  (%s)", items[i].source);
         dstr_append_char(&out, '\n');
      }
   if (obs == 0)
      dstr_append_str(&out, "(none)\n");

   diagnosis_ranking_t rankings[64];
   int r = db1_diagnose_rank_hypotheses(diag_id, rankings, 64);
   dstr_append_str(&out, "\n## Hypotheses (ranked by evidence)\n");
   if (r == 0)
      dstr_append_str(&out, "(none)\n");
   for (int i = 0; i < r; i++)
   {
      dstr_appendf(&out, "### H%d (id=%d, confidence %.2f): %s\n", i + 1, rankings[i].hypothesis.id,
                   rankings[i].confidence, rankings[i].hypothesis.content);
      for (int j = 0; j < total; j++)
      {
         if (items[j].parent_id != rankings[i].hypothesis.id)
            continue;
         if (strcmp(items[j].kind, "evidence_for") == 0)
            dstr_appendf(&out, "  + [%s] %s\n", rank_label(items[j].evidence_rank),
                         items[j].content);
         else if (strcmp(items[j].kind, "evidence_against") == 0)
            dstr_appendf(&out, "  - [%s] %s\n", rank_label(items[j].evidence_rank),
                         items[j].content);
         else if (strcmp(items[j].kind, "probe") == 0)
            dstr_appendf(&out, "  -> probe: %s\n", items[j].content);
      }
   }

   diagnosis_probe_suggestion_t probes[DIAG_MAX_SUGGEST];
   int probe_count = db1_diagnose_suggest_probes(diag_id, probes, DIAG_MAX_SUGGEST);
   if (probe_count > 0)
   {
      dstr_append_str(&out, "\n## Suggested Probes\n");
      for (int i = 0; i < probe_count; i++)
         dstr_appendf(&out, "%d. %s\n", i + 1, probes[i].suggestion);
   }

   return dstr_steal(&out);
}
