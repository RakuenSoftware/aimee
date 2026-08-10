/* retention_probe.c: how much load-bearing detail survives a compaction boundary?
 *
 * Runs BOTH summary derivations over the same corpus and reports, per fixture and in
 * aggregate, how many PLANTED facts survive verbatim into the summary.
 *
 * Why planted facts, and not extracted ones: if ground truth were produced by
 * coord_closet (the very extractor the record path uses), the record path would score
 * 100% by construction and the number would mean nothing. That is the
 * assertion-that-tracks-instead-of-checking failure. Every expected string here is
 * written down by hand in corpus.json, independent of both derivations, and matched by
 * plain substring search.
 *
 * The corpus is deliberately balanced: some categories favour the legacy prose scan
 * (keyworded decisions, keyworded errors), some favour the record path (identifiers,
 * register-tagged turns), and one - untagged-realistic - is the case the record path is
 * expected to LOSE, because register tagging is off by default so a real transcript
 * carries no [verdict]/[hazard] tags at all.
 *
 * This measures RETENTION only. It says nothing about rounds-to-resume, which needs
 * live agents; see docs/proposals/pending/compaction-quality-baseline.md.
 *
 * Exit status is 0 whenever the run completed. It is a measurement, not a gate - a
 * derivation scoring badly is a result to read, not a build failure.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aimee.h"
#include "session_compact.h"
#include "cJSON.h"

/* Compaction only engages once the array is longer than the anchor plus the retained
 * tail, and only the middle is summarised. Padding keeps the fixture's own turns inside
 * the summarised region rather than the verbatim tail - otherwise a fact would "survive"
 * simply by never having been compacted, which measures nothing. */
#define PAD_PAIRS 8

static cJSON *msg(const char *role, const char *content)
{
   cJSON *m = cJSON_CreateObject();
   cJSON_AddStringToObject(m, "role", role);
   cJSON_AddStringToObject(m, "content", content);
   return m;
}

static cJSON *build_messages(const cJSON *fixture)
{
   cJSON *arr = cJSON_CreateArray();
   const cJSON *msgs = cJSON_GetObjectItemCaseSensitive((cJSON *)fixture, "messages");
   const cJSON *m = NULL;
   cJSON_ArrayForEach(m, msgs)
   {
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "role"));
      const char *content = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)m, "content"));
      cJSON_AddItemToArray(arr, msg(role ? role : "user", content ? content : ""));
   }
   for (int i = 0; i < PAD_PAIRS; i++)
   {
      char u[96], a[96];
      snprintf(u, sizeof(u), "Filler user turn %d, carrying nothing worth conserving.", i);
      snprintf(a, sizeof(a), "Filler assistant turn %d, carrying nothing worth conserving.", i);
      cJSON_AddItemToArray(arr, msg("user", u));
      cJSON_AddItemToArray(arr, msg("assistant", a));
   }
   return arr;
}

/* Returns the number of planted facts present verbatim in `summary`, and writes the
 * misses into `missed` (comma separated, truncated) so a low score is explainable
 * rather than just a number. */
static int count_retained(const cJSON *planted, const char *summary, char *missed, size_t cap)
{
   int kept = 0;
   size_t pos = 0;
   if (cap)
      missed[0] = '\0';
   const cJSON *p = NULL;
   cJSON_ArrayForEach(p, planted)
   {
      const char *want = cJSON_GetStringValue((cJSON *)p);
      if (!want || !want[0])
         continue;
      if (strstr(summary, want))
      {
         kept++;
         continue;
      }
      if (cap && pos + 3 < cap)
      {
         int n = snprintf(missed + pos, cap - pos, "%s%.60s", pos ? ", " : "", want);
         if (n > 0)
            pos += (size_t)n < cap - pos ? (size_t)n : cap - pos - 1;
      }
   }
   return kept;
}

static int run_one(const cJSON *fixture, int from_record, char *summary_out, size_t summary_cap)
{
   cJSON *arr = build_messages(fixture);
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = from_record;

   session_compact_result_t result;
   int rc = session_compact(arr, &cfg, &result);
   int compacted = (rc == 0 && result.compacted);
   if (compacted)
      snprintf(summary_out, summary_cap, "%s", result.summary);
   else
      summary_out[0] = '\0';
   cJSON_Delete(arr);
   return compacted;
}

int main(int argc, char **argv)
{
   const char *path = argc > 1 ? argv[1] : "benchmarks/compaction-quality/corpus.json";
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      fprintf(stderr, "retention_probe: cannot open %s\n", path);
      return 2;
   }
   fseek(f, 0, SEEK_END);
   long len = ftell(f);
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)len + 1);
   if (!buf || fread(buf, 1, (size_t)len, f) != (size_t)len)
   {
      fprintf(stderr, "retention_probe: cannot read %s\n", path);
      fclose(f);
      free(buf);
      return 2;
   }
   buf[len] = '\0';
   fclose(f);

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
   {
      fprintf(stderr, "retention_probe: cannot parse %s\n", path);
      return 2;
   }
   cJSON *fixtures = cJSON_GetObjectItemCaseSensitive(root, "fixtures");
   if (!cJSON_IsArray(fixtures))
   {
      fprintf(stderr, "retention_probe: no fixtures array\n");
      cJSON_Delete(root);
      return 2;
   }

   printf("%-34s %-20s %8s %8s   %s\n", "fixture", "category", "legacy", "record", "expect");
   printf("---------------------------------------------------------------------------------"
          "-----\n");

   int tot_planted = 0, tot_legacy = 0, tot_record = 0;
   int skipped = 0;
   static char sum_legacy[SESSION_COMPACT_SUMMARY_MAX];
   static char sum_record[SESSION_COMPACT_SUMMARY_MAX];
   char missed_legacy[512], missed_record[512];

   const cJSON *fx = NULL;
   cJSON_ArrayForEach(fx, fixtures)
   {
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)fx, "id"));
      const char *cat = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)fx, "category"));
      const char *expect = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)fx, "expect"));
      cJSON *planted = cJSON_GetObjectItem((cJSON *)fx, "planted");
      int n_planted = cJSON_IsArray(planted) ? cJSON_GetArraySize(planted) : 0;
      if (!id || n_planted == 0)
         continue;

      int c1 = run_one(fx, 0, sum_legacy, sizeof(sum_legacy));
      int c2 = run_one(fx, 1, sum_record, sizeof(sum_record));
      if (!c1 || !c2)
      {
         /* No boundary means nothing was measured. Report it rather than scoring 0,
          * which would look like total loss. */
         printf("%-34s %-20s %8s %8s   %s\n", id, cat ? cat : "?", "no-compact", "no-compact",
                expect ? expect : "?");
         skipped++;
         continue;
      }

      int kept_legacy = count_retained(planted, sum_legacy, missed_legacy, sizeof(missed_legacy));
      int kept_record = count_retained(planted, sum_record, missed_record, sizeof(missed_record));

      char l[32], r[32];
      snprintf(l, sizeof(l), "%d/%d", kept_legacy, n_planted);
      snprintf(r, sizeof(r), "%d/%d", kept_record, n_planted);
      printf("%-34s %-20s %8s %8s   %s\n", id, cat ? cat : "?", l, r, expect ? expect : "?");
      if (kept_legacy < n_planted && missed_legacy[0])
         printf("      legacy missed: %s\n", missed_legacy);
      if (kept_record < n_planted && missed_record[0])
         printf("      record missed: %s\n", missed_record);

      tot_planted += n_planted;
      tot_legacy += kept_legacy;
      tot_record += kept_record;
   }

   printf("---------------------------------------------------------------------------------"
          "-----\n");
   printf("TOTAL retained: legacy %d/%d (%.1f%%)   record %d/%d (%.1f%%)   skipped %d\n",
          tot_legacy, tot_planted, tot_planted ? 100.0 * tot_legacy / tot_planted : 0.0,
          tot_record, tot_planted, tot_planted ? 100.0 * tot_record / tot_planted : 0.0, skipped);

   cJSON_Delete(root);
   return 0;
}
