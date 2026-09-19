/* Real Go context and diagnostics behind the remaining benchmark host connection. */
#include "aimee.h"
#include "modules/benchmarks/agent_eval_internal.h"
#include "json_fluent.h"
#include "support/module_runtime_fixture.h"
#include <assert.h>

static int unavailable;
static const char *malformed;

int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.runtime"));
   const char *operation = jo_cstr(args, "operation");
   assert(!strcmp(operation, "benchmark-context") ||
          !strcmp(operation, "benchmark-hard-negative") || !strcmp(operation, "benchmark-miss") ||
          !strcmp(operation, "benchmark-score"));
   assert(!cJSON_HasObjectItem(args, "project"));
   if (unavailable)
   {
      *result = NULL;
      return -1;
   }
   if (malformed)
   {
      *result = cJSON_Parse(malformed);
      return 1;
   }
   return module_runtime_fixture_call(args, result);
}
static char *read_file(FILE *fp)
{
   assert(fflush(fp) == 0);
   long size = ftell(fp);
   assert(size >= 0);
   rewind(fp);
   char *text = calloc((size_t)size + 1, 1);
   assert(text && fread(text, 1, (size_t)size, fp) == (size_t)size);
   return text;
}

static void test_diagnostics(void)
{
   eval_task_t task = {0};
   agent_result_t result = {0};
   snprintf(task.name, sizeof(task.name), "task");
   snprintf(task.prompt, sizeof(task.prompt), "fixture");
   snprintf(task.success_check_value, sizeof(task.success_check_value), "expected");
   result.response = "answer\nquoted";
   FILE *fp = tmpfile();
   assert(fp && mem_eval_write_hard_negative(fp, "suite", &task, &result) == 0);
   char *text = read_file(fp);
   assert(strstr(text, "\"id\":9007199254740993"));
   cJSON *artifact = cJSON_Parse(text);
   const cJSON *candidate =
       cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(artifact, "top_candidates"), 0);
   assert(strlen(jo_cstr(candidate, "content")) == 6300);
   assert(!strcmp(jo_cstr(artifact, "got"), result.response));
   cJSON_Delete(artifact);
   free(text);
   fclose(fp);

   snprintf(task.prompt, sizeof(task.prompt), "failure");
   fp = tmpfile();
   assert(fp && mem_eval_write_hard_negative(fp, "suite", &task, &result) == 0);
   text = read_file(fp);
   assert(strstr(text, "\"memory_error\":\"memory retrieval index unavailable\""));
   free(text);
   fclose(fp);

   const char *bad_lines[] = {"{}", "{\"status\":\"ok\",\"line\":12}",
                              "{\"status\":\"ok\",\"line\":\"injected\\nline\"}"};
   for (size_t i = 0; i < sizeof(bad_lines) / sizeof(bad_lines[0]); i++)
   {
      malformed = bad_lines[i];
      fp = tmpfile();
      assert(fp && mem_eval_write_hard_negative(fp, "suite", &task, &result) == -1);
      assert(ftell(fp) == 0);
      fclose(fp);
   }
   malformed = NULL;
   unavailable = 1;
   fp = tmpfile();
   assert(fp && mem_eval_write_hard_negative(fp, "suite", &task, &result) == -1);
   assert(ftell(fp) == 0);
   fclose(fp);
   unavailable = 0;
   fp = fopen("/dev/full", "w");
   assert(fp && mem_eval_write_hard_negative(fp, "suite", &task, &result) == -1);
   fclose(fp);

   mem_eval_case_t ecase = {0};
   snprintf(ecase.query, sizeof(ecase.query), "fixture");
   ecase.expected_ids[0] = INT64_C(9007199254740993);
   ecase.n_expected = 1;
   int reported = 0, misses = 0, scanned = 0, buckets[8] = {0};
   fp = tmpfile();
   assert(fp);
   mem_eval_print_miss_report(fp, &ecase, 1, 0, 1, &reported, &misses, &buckets[0], &buckets[1],
                              &buckets[2], &buckets[3], &buckets[4], &buckets[5], &buckets[6],
                              &buckets[7], NULL, "test", &scanned);
   text = read_file(fp);
   assert(reported == 1 && misses == 1 && scanned == 1);
   assert(strstr(text, "MISS rank=1") && strstr(text, "expected_id=9007199254740993 key=large"));
   assert(strstr(text, "[1]9007199254740993:large") && strlen(text) > 6300);
   free(text);
   fclose(fp);
   fp = tmpfile();
   assert(fp);
   mem_eval_print_miss_report(fp, &ecase, 1, 1, 1, &reported, &misses, &buckets[0], &buckets[1],
                              &buckets[2], &buckets[3], &buckets[4], &buckets[5], &buckets[6],
                              &buckets[7], NULL, "test", &scanned);
   assert(reported == 1 && misses == 1 && scanned == 2 && ftell(fp) == 0);
   fclose(fp);
   malformed =
       "{\"status\":\"ok\",\"rank\":1.5,\"is_miss\":true,\"bucket\":\"missing\",\"text\":\"bad\"}";
   fp = tmpfile();
   assert(fp);
   mem_eval_print_miss_report(fp, &ecase, 1, 0, 10, &reported, &misses, &buckets[0], &buckets[1],
                              &buckets[2], &buckets[3], &buckets[4], &buckets[5], &buckets[6],
                              &buckets[7], NULL, "test", &scanned);
   text = read_file(fp);
   assert(reported == 1 && misses == 1 && scanned == 2 && strstr(text, "ERROR query="));
   free(text);
   fclose(fp);
   malformed = NULL;
}

static void test_scoring(void)
{
   mem_eval_case_t ecase = {0};
   snprintf(ecase.query, sizeof(ecase.query), "fixture");
   ecase.expected_ids[0] = INT64_C(9007199254740993);
   ecase.n_expected = 1;
   mem_eval_direct_trace_t trace;
   assert(mem_eval_score_case(&ecase, &trace) == 0);
   assert(trace.n_retrieved == 2 && trace.retrieved_ids[0] == ecase.expected_ids[0]);
   assert(trace.mrr == 1 && trace.ndcg_5 == 1 && trace.ndcg_10 == 1 && trace.recall_5 == 1 &&
          trace.recall_10 == 1 && trace.latency_ms >= 0);
   FILE *progress = tmpfile();
   assert(progress);
   mem_eval_append_direct_progress_row(progress, "test", "q1", "category", "fact", "question",
                                       "answer", &trace);
   char *line = read_file(progress);
   assert(strstr(line, "\"retrieved_ids\":[9007199254740993,2]"));
   assert(strstr(line, "\"verdict\":\"CORRECT\""));
   free(line);
   fclose(progress);
   mem_eval_support_case_t cases[2] = {0};
   snprintf(cases[0].query, sizeof(cases[0].query), "fixture");
   cases[0].relevant_ids[0] = ecase.expected_ids[0];
   cases[0].n_relevant = 1;
   snprintf(cases[1].query, sizeof(cases[1].query), "empty");
   cases[1].relevant_ids[0] = 999;
   cases[1].n_relevant = 1;
   mem_eval_scores_t scores;
   mem_eval_latency_t latency;
   assert(mem_eval_run_support_with_latency(cases, 2, &scores, &latency) == 0);
   assert(scores.n_cases == 2 && scores.mrr == .5 && scores.ndcg_5 == .5 && scores.ndcg_10 == .5 &&
          scores.recall_5 == .5 && scores.recall_10 == .5);
   int64_t relevant[128];
   for (int i = 0; i < 128; i++)
      relevant[i] = ecase.expected_ids[0];
   assert(mem_eval_score_retrieval("fixture", relevant, 128, &trace) == 0);
   assert(trace.recall_5 == 1.0 / 128);
   mem_eval_direct_trace_t zero = {0};
   memset(&trace, 0xff, sizeof(trace));
   assert(mem_eval_score_retrieval("failure", relevant, 128, &trace) == -1);
   assert(!memcmp(&trace, &zero, sizeof(trace)));
   const char *bad[] = {
       "{}",
       "{\"status\":\"ok\",\"retrieved_ids\":[9007199254740993],\"mrr\":1,\"ndcg_5\":1,\"ndcg_10\":"
       "1,\"recall_5\":1,\"recall_10\":1}",
       "{\"status\":\"ok\",\"retrieved_ids\":[\"9223372036854775808\"],\"mrr\":1,\"ndcg_5\":1,"
       "\"ndcg_10\":1,\"recall_5\":1,\"recall_10\":1}",
       "{\"status\":\"ok\",\"retrieved_ids\":[\"01\"],\"mrr\":1,\"ndcg_5\":1,\"ndcg_10\":1,"
       "\"recall_5\":1,\"recall_10\":1}",
       "{\"status\":\"ok\",\"retrieved_ids\":[\"1\"],\"mrr\":2,\"ndcg_5\":1,\"ndcg_10\":1,\"recall_"
       "5\":1,\"recall_10\":1}",
       "{\"status\":\"ok\",\"retrieved_ids\":[\"1\"],\"mrr\":1,\"ndcg_5\":1,\"ndcg_10\":1,\"recall_"
       "5\":-1,\"recall_10\":1}",
   };
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      malformed = bad[i];
      memset(&trace, 0xff, sizeof(trace));
      assert(mem_eval_score_case(&ecase, &trace) == -1);
      assert(!memcmp(&trace, &zero, sizeof(trace)));
   }
   malformed = NULL;
   unavailable = 1;
   assert(mem_eval_score_case(&ecase, &trace) == -1);
   assert(!memcmp(&trace, &zero, sizeof(trace)));
   unavailable = 0;
}

int main(void)
{
   assert(setenv("AIMEE_TEST_BENCHMARK_FIXTURE", "1", 1) == 0);
   char context[16384];
   int tokens = -1;
   assert(mem_eval_build_retrieval_context("fixture", 1, 4000, context, sizeof(context), &tokens) ==
          1);
   assert(strlen(context) == 6305 && tokens == 1577);
   assert(!memcmp(context, "[1] ", 4));
   for (int i = 0; i < 2100; i++)
      assert(!memcmp(context + 4 + 3 * i, "界", 3));
   assert(context[6304] == '\n');
   char small[31];
   assert(mem_eval_build_retrieval_context("fixture", 2, 20, small, sizeof(small), &tokens) == 1);
   assert(strlen(small) < sizeof(small) && tokens <= 20);
   assert(strstr(small, "...\n"));
   assert(mem_eval_build_retrieval_context("empty", 0, 0, context, sizeof(context), &tokens) == 0);
   assert(context[0] == '\0' && tokens == 0);
   strcpy(context, "stale");
   tokens = 99;
   assert(mem_eval_build_retrieval_context("failure", 1, 20, context, sizeof(context), &tokens) ==
          -1);
   assert(context[0] == '\0' && tokens == 0);
   unavailable = 1;
   assert(mem_eval_build_retrieval_context("fixture", 1, 20, context, sizeof(context), &tokens) ==
          -1);
   unavailable = 0;
   const char *bad[] = {"{\"status\":\"ok\",\"context\":\"x\",\"tokens\":0,\"kept\":0}",
                        "{}",
                        "{\"status\":\"ok\",\"context\":\"x\",\"tokens\":21,\"kept\":1}",
                        "{\"status\":\"ok\",\"context\":\"x\",\"tokens\":1.5,\"kept\":1}",
                        "{\"status\":\"ok\",\"context\":\"x\",\"tokens\":1,\"kept\":2}",
                        "{\"status\":\"ok\",\"context\":\"123456789\",\"tokens\":3,\"kept\":1}"};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      malformed = bad[i];
      strcpy(context, "stale");
      tokens = 99;
      assert(mem_eval_build_retrieval_context("fixture", 1, 20, context, 9, &tokens) == -1);
      assert(context[0] == '\0' && tokens == 0);
   }
   malformed = NULL;
   assert(mem_eval_build_retrieval_context(NULL, 1, 20, context, sizeof(context), NULL) == -1);
   test_diagnostics();
   test_scoring();
   puts(
       "benchmark transport: real Go context, diagnostics, scoring, exact IDs and failures passed");
   return 0;
}
