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
#include "context_reduce.h"
#include "cJSON.h"

/* Compaction only engages once the array is longer than the anchor plus the retained
 * tail, and only the middle is summarised. Padding keeps the fixture's own turns inside
 * the summarised region rather than the verbatim tail - otherwise a fact would "survive"
 * simply by never having been compacted, which measures nothing.
 *
 * The padding must also be BULKY. With short filler the whole corpus is smaller than the
 * retained tail plus a summary header, so every arm comes out LARGER than the unreduced
 * baseline and the fold "wins" 100% recall by keeping everything - a measurement of the
 * fixture size, not of the mechanism. The cost axis catches that, and these numbers are
 * what stop it happening: enough turns and enough bytes per turn that reduction has
 * something real to remove. */
#define PAD_PAIRS      14
#define PAD_FILLER_LEN 700

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
      char u[PAD_FILLER_LEN + 64], a[PAD_FILLER_LEN + 64];
      int un = snprintf(u, sizeof(u), "Filler user turn %d, carrying nothing worth conserving. ", i);
      int an = snprintf(a, sizeof(a),
                        "Filler assistant turn %d, carrying nothing worth conserving. ", i);
      /* Pad with words rather than a repeated character: a run of 'x' is not
       * representative of the token volume a real turn carries. */
      while (un < PAD_FILLER_LEN)
         un += snprintf(u + un, sizeof(u) - (size_t)un, "routine detail %d ", un);
      while (an < PAD_FILLER_LEN)
         an += snprintf(a + an, sizeof(a) - (size_t)an, "routine detail %d ", an);
      cJSON_AddItemToArray(arr, msg("user", u));
      cJSON_AddItemToArray(arr, msg("assistant", a));
   }
   return arr;
}

/* Count how many of `items` appear verbatim in `summary`, listing the interesting side
 * into `report`.
 *
 * `list_hits` selects WHICH side is interesting, and the two callers want opposite
 * things: for planted facts the useful diagnostic is what was MISSED, for distractors it
 * is what was PULLED IN. Reporting the wrong side prints an empty string exactly when
 * the score is worst, which is when the explanation matters most. */
static int count_matches(const cJSON *items, const char *summary, int list_hits, char *report,
                         size_t cap)
{
   int found = 0;
   size_t pos = 0;
   if (report && cap)
      report[0] = '\0';
   const cJSON *p = NULL;
   cJSON_ArrayForEach(p, items)
   {
      const char *want = cJSON_GetStringValue((cJSON *)p);
      if (!want || !want[0])
         continue;
      int hit = strstr(summary, want) != NULL;
      if (hit)
         found++;
      if (hit != list_hits)
         continue;
      if (report && cap && pos + 3 < cap)
      {
         int n = snprintf(report + pos, cap - pos, "%s%.60s", pos ? ", " : "", want);
         if (n > 0)
            pos += (size_t)n < cap - pos ? (size_t)n : cap - pos - 1;
      }
   }
   return found;
}

/* Score the POST-REDUCTION TRANSCRIPT, not a summary string.
 *
 * The fold produces no summary — its artefact is the whole reduced message array — so
 * scoring `result.summary` could not compare the two mechanisms at all. Serializing what
 * the model would actually receive is also the fairer question: a fact is retained if it
 * is still VISIBLE, wherever it sits. Returns a malloc'd string (caller frees), or NULL
 * when no reduction happened. */
static char *run_compactor(const cJSON *fixture, int from_record)
{
   cJSON *arr = build_messages(fixture);
   session_compact_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.from_record = from_record;

   session_compact_result_t result;
   int rc = session_compact(arr, &cfg, &result);
   char *out = NULL;
   if (rc == 0 && result.compacted)
      out = cJSON_PrintUnformatted(arr); /* boundary summary + retained tail */
   cJSON_Delete(arr);
   return out;
}

/* The FOLD arm: the continuous, freeze-aware mechanism, which is default-off in
 * production while the 80% cliff compactor is default-on. This is the comparison that
 * gates flipping fold_enabled. */
static char *run_fold(const cJSON *fixture)
{
   cJSON *arr = build_messages(fixture);
   reduce_config_t cfg;
   memset(&cfg, 0, sizeof(cfg));
   cfg.delegate_seam = 1;
   cfg.history_fold = 1;
   cfg.fold.closet.enabled = 1;

   reduce_state_t st;
   memset(&st, 0, sizeof(st));
   reduce_result_t res;
   memset(&res, 0, sizeof(res));

   char *out = NULL;
   if (context_reduce(arr, NULL, "gpt-4o", "probe", REDUCE_SEAM_DELEGATE, &cfg, &st, &res) == 0 &&
       res.mutated && res.messages)
      out = cJSON_PrintUnformatted(res.messages);
   context_reduce_result_free(&res);
   fold_recall_index_free(&st.recall);
   cJSON_Delete(arr);
   return out;
}

/* Baseline: what the transcript costs with NO reduction at all. Retention without cost
 * is meaningless — keeping everything scores 100% and saves nothing. */
static char *run_none(const cJSON *fixture)
{
   cJSON *arr = build_messages(fixture);
   char *out = cJSON_PrintUnformatted(arr);
   cJSON_Delete(arr);
   return out;
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

   printf("%-30s %7s %7s %7s   %7s %7s %7s\n", "fixture", "L:keep", "R:keep", "F:keep", "L:FP",
          "R:FP", "F:FP");
   printf("---------------------------------------------------------------------------------"
          "-----\n");

   int tot_planted = 0, tot_legacy = 0, tot_record = 0, tot_fold = 0;
   int tot_distract = 0, tot_fp_legacy = 0, tot_fp_record = 0, tot_fp_fold = 0;
   long bytes_none = 0, bytes_legacy = 0, bytes_record = 0, bytes_fold = 0;
   int skipped = 0;
   char missed_legacy[512], missed_record[512];

   const cJSON *fx = NULL;
   cJSON_ArrayForEach(fx, fixtures)
   {
      const char *id = cJSON_GetStringValue(cJSON_GetObjectItem((cJSON *)fx, "id"));
      cJSON *planted = cJSON_GetObjectItem((cJSON *)fx, "planted");
      cJSON *distract = cJSON_GetObjectItem((cJSON *)fx, "distractors");
      int n_planted = cJSON_IsArray(planted) ? cJSON_GetArraySize(planted) : 0;
      int n_distract = cJSON_IsArray(distract) ? cJSON_GetArraySize(distract) : 0;
      /* A fixture may plant nothing and only carry distractors: there, a perfect score
       * is retaining NOTHING, which is a precision measurement rather than a recall one. */
      if (!id || (n_planted == 0 && n_distract == 0))
         continue;

      char *t_legacy = run_compactor(fx, 0);
      char *t_record = run_compactor(fx, 1);
      char *t_fold = run_fold(fx);
      char *t_none = run_none(fx);
      if (!t_legacy || !t_record)
      {
         /* No boundary means nothing was measured. Report it rather than scoring 0,
          * which would look like total loss. */
         printf("%-30s %7s %7s %7s   %7s %7s %7s\n", id, "n/c", "n/c", "n/c", "n/c", "n/c", "n/c");
         skipped++;
         free(t_legacy);
         free(t_record);
         free(t_fold);
         free(t_none);
         continue;
      }
      /* The fold has its own engagement conditions (a clean user-turn boundary, a net
       * gain). When it does not fire, the transcript is unreduced — scoring that as a
       * win would credit the fold for keeping everything. */
      const char *fold_txt = t_fold ? t_fold : "";

      int kept_legacy = count_matches(planted, t_legacy, 0, missed_legacy, sizeof(missed_legacy));
      int kept_record = count_matches(planted, t_record, 0, missed_record, sizeof(missed_record));
      int kept_fold = count_matches(planted, fold_txt, 0, NULL, 0);
      /* A retained DISTRACTOR is a false positive: noise the derivation dragged in. Same
       * containment check as recall, read with the opposite sign — and listing the HITS,
       * since those are what needs explaining. */
      char pulled_legacy[512], pulled_record[512];
      int fp_legacy = count_matches(distract, t_legacy, 1, pulled_legacy, sizeof(pulled_legacy));
      int fp_record = count_matches(distract, t_record, 1, pulled_record, sizeof(pulled_record));
      int fp_fold = count_matches(distract, fold_txt, 1, NULL, 0);

      char l[32], r[32], f[32], fl[32], fr[32], ff[32];
      snprintf(l, sizeof(l), "%d/%d", kept_legacy, n_planted);
      snprintf(r, sizeof(r), "%d/%d", kept_record, n_planted);
      snprintf(f, sizeof(f), t_fold ? "%d/%d" : "no-fold", kept_fold, n_planted);
      snprintf(fl, sizeof(fl), "%d/%d", fp_legacy, n_distract);
      snprintf(fr, sizeof(fr), "%d/%d", fp_record, n_distract);
      snprintf(ff, sizeof(ff), t_fold ? "%d/%d" : "-", fp_fold, n_distract);
      printf("%-30s %7s %7s %7s   %7s %7s %7s\n", id, l, r, f, fl, fr, ff);
      if (kept_legacy < n_planted && missed_legacy[0])
         printf("      legacy missed: %s\n", missed_legacy);
      if (kept_record < n_planted && missed_record[0])
         printf("      record missed: %s\n", missed_record);
      if (fp_legacy > 0)
         printf("      legacy PULLED IN NOISE: %s\n", pulled_legacy);
      if (fp_record > 0)
         printf("      record PULLED IN NOISE: %s\n", pulled_record);

      tot_planted += n_planted;
      tot_legacy += kept_legacy;
      tot_record += kept_record;
      tot_fold += kept_fold;
      tot_distract += n_distract;
      tot_fp_legacy += fp_legacy;
      tot_fp_record += fp_record;
      tot_fp_fold += fp_fold;
      bytes_none += t_none ? (long)strlen(t_none) : 0;
      bytes_legacy += (long)strlen(t_legacy);
      bytes_record += (long)strlen(t_record);
      bytes_fold += (long)strlen(fold_txt);

      free(t_legacy);
      free(t_record);
      free(t_fold);
      free(t_none);
   }

   printf("---------------------------------------------------------------------------------"
          "-----\n");
   printf("RECALL    (planted retained, higher better): legacy %d/%d (%.1f%%)   record %d/%d "
          "(%.1f%%)   fold %d/%d (%.1f%%)\n",
          tot_legacy, tot_planted, tot_planted ? 100.0 * tot_legacy / tot_planted : 0.0,
          tot_record, tot_planted, tot_planted ? 100.0 * tot_record / tot_planted : 0.0, tot_fold,
          tot_planted, tot_planted ? 100.0 * tot_fold / tot_planted : 0.0);
   printf("PRECISION (distractors pulled in, LOWER better): legacy %d/%d (%.1f%%)   record %d/%d "
          "(%.1f%%)   fold %d/%d (%.1f%%)\n",
          tot_fp_legacy, tot_distract, tot_distract ? 100.0 * tot_fp_legacy / tot_distract : 0.0,
          tot_fp_record, tot_distract, tot_distract ? 100.0 * tot_fp_record / tot_distract : 0.0,
          tot_fp_fold, tot_distract, tot_distract ? 100.0 * tot_fp_fold / tot_distract : 0.0);
   /* Retention without cost is meaningless: keeping everything scores 100% and saves
    * nothing. Sizes are of the serialized transcript the model would receive. */
   printf("COST      (transcript bytes, LOWER better): none %ld   legacy %ld (%.0f%%)   record "
          "%ld (%.0f%%)   fold %ld (%.0f%%)\n",
          bytes_none, bytes_legacy, bytes_none ? 100.0 * bytes_legacy / bytes_none : 0.0,
          bytes_record, bytes_none ? 100.0 * bytes_record / bytes_none : 0.0, bytes_fold,
          bytes_none ? 100.0 * bytes_fold / bytes_none : 0.0);
   printf("skipped (no compaction boundary): %d\n", skipped);
   printf("\nRecall alone cannot separate 'kept what matters' from 'kept everything'; read it\n"
          "with COST. Ground truth is planted by hand in corpus.json, never extracted by\n"
          "coord_closet or fold_register - deriving it with the code under test would score\n"
          "that derivation perfectly by construction.\n"
          "\n"
          "READING THE FOLD COLUMN. The compactor SUMMARISES: a distractor it retains was\n"
          "asserted as a fact under ## Key Decisions or ## Blocked, which is a real false\n"
          "positive. The fold COMPRESSES: a distractor survives because its TEXT survives in\n"
          "a skeleton excerpt, not because anything classified it. So F:FP is not comparable\n"
          "to L:FP / R:FP - the precision axis was built for extraction and does not transfer\n"
          "to a compressor. What the fold column does say honestly is its RECALL and its\n"
          "COST: how much it keeps, and what that costs.\n");

   cJSON_Delete(root);
   return 0;
}
