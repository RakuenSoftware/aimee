/* learning_implicit_replay.c: replay implicit-signal fixtures through the REAL
 * detector and emit predictions for benchmarks/learning/learning_replay.py.
 *
 * Closes the "remaining wire" for the learning-router rollout flags: the
 * harness grades an *injected* predictions file so it never duplicates the C
 * detector logic. This tool produces that file by running the actual
 * classifier, binding the metric to the real build.
 *
 * Scope: the two pure-text citation heuristics whose decision is the
 * deterministic per-turn classifier dogfood_classify_next_turn() —
 *   citation_then_repair       fires iff the turn classifies as REPAIR
 *   citation_then_continuation fires iff the turn classifies as CONTINUATION
 * The three stateful heuristics (repeat_question, repeated_correction,
 * workflow_repetition) need session/DB state and a live router; they are
 * skipped here (a fixture row for them produces no prediction line).
 *
 * Usage:
 *   learning-implicit-replay FIXTURE.jsonl [FIXTURE2.jsonl ...] > predictions.jsonl
 * then grade (same citation subset, file order → positional alignment):
 *   python3 benchmarks/learning/learning_replay.py FIXTURE.jsonl \
 *       --heuristics citation_then_repair,citation_then_continuation \
 *       --predictions predictions.jsonl
 *
 * Each emitted line is {"predicted": <bool>}; one line per citation fixture
 * row, in file order, so it aligns positionally with the --heuristics-filtered
 * fixtures.
 */
#include "cJSON.h"
#include "dogfood.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int replay_file(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
   {
      fprintf(stderr, "learning-implicit-replay: cannot open %s\n", path);
      return -1;
   }
   char *line = NULL;
   size_t cap = 0;
   ssize_t len;
   while ((len = getline(&line, &cap, f)) > 0)
   {
      if (len <= 1)
         continue;
      cJSON *row = cJSON_Parse(line);
      if (!row)
         continue;
      const cJSON *h = cJSON_GetObjectItemCaseSensitive(row, "heuristic");
      const cJSON *t = cJSON_GetObjectItemCaseSensitive(row, "user_text");
      if (cJSON_IsString(h) && cJSON_IsString(t))
      {
         int repair = strcmp(h->valuestring, "citation_then_repair") == 0;
         int cont = strcmp(h->valuestring, "citation_then_continuation") == 0;
         if (repair || cont)
         {
            dogfood_autolabel_kind_t kind = dogfood_classify_next_turn(t->valuestring);
            int predicted = repair ? (kind == DOGFOOD_AUTOLABEL_REPAIR)
                                   : (kind == DOGFOOD_AUTOLABEL_CONTINUATION);
            printf("{\"predicted\": %s}\n", predicted ? "true" : "false");
         }
      }
      cJSON_Delete(row);
   }
   free(line);
   fclose(f);
   return 0;
}

int main(int argc, char **argv)
{
   if (argc < 2)
   {
      fprintf(stderr, "usage: %s FIXTURE.jsonl [FIXTURE2.jsonl ...]\n", argv[0]);
      return 2;
   }
   for (int i = 1; i < argc; i++)
      if (replay_file(argv[i]) != 0)
         return 1;
   return 0;
}
