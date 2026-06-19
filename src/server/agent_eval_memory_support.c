/* _GNU_SOURCE: strcasestr/memmem are GNU extensions; declare them before any
 * libc header so gcc-12 (the container toolchain) does not implicit-decl + -Werror. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* agent_eval_memory_support.c: corpus/QA benchmark helpers and temp DB support.
 * Split from agent_eval.c to keep the core eval harness under the lint cap. */
#include "aimee.h"
#include "agent_eval.h"
#include "agent_eval_internal.h"
#include "agent_config.h"
#include "agent_exec.h"
#include "config.h"
#include "memory.h"
#include "lifecycle.h"
#include "eval_support.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* --- Corpus-based memory retrieval eval --- */

/* Internal: read entire file into a heap-allocated string. Caller frees. */
char *slurp_file_eval(const char *path)
{
   FILE *f = fopen(path, "r");
   if (!f)
      return NULL;
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   fseek(f, 0, SEEK_SET);
   if (sz <= 0)
   {
      fclose(f);
      return NULL;
   }
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t nread = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[nread] = '\0';
   return buf;
}

/* Fixture ID → DB ID mapping tables and the other internal value types used
 * by the eval runners live in agent_eval_internal.h so benchmarks can see
 * them. */

int64_t fid_lookup(const corpus_fid_entry_t *map, int n_map, const char *fid)
{
   for (int i = 0; i < n_map; i++)
      if (strcmp(map[i].fid, fid) == 0)
         return map[i].db_id;
   return 0;
}

int locomo_fid_lookup(const locomo_fid_entry_t *map, int n_map, const char *fid, int64_t *db_id_out,
                      char *scope_out, size_t scope_len)
{
   for (int i = 0; i < n_map; i++)
   {
      if (strcmp(map[i].fid, fid) == 0)
      {
         if (db_id_out)
            *db_id_out = map[i].db_id;
         if (scope_out && scope_len > 0)
            snprintf(scope_out, scope_len, "%s", map[i].scope);
         return 1;
      }
   }
   return 0;
}

mem_eval_scope_bucket_t *mem_eval_get_scope_bucket(mem_eval_scope_bucket_t *buckets,
                                                   int *bucket_count, int max_buckets,
                                                   const char *scope)
{
   if (!buckets || !bucket_count || !scope || !scope[0])
      return NULL;
   for (int i = 0; i < *bucket_count; i++)
      if (strcmp(buckets[i].scope, scope) == 0)
         return &buckets[i];
   if (*bucket_count >= max_buckets)
      return NULL;
   snprintf(buckets[*bucket_count].scope, sizeof(buckets[*bucket_count].scope), "%s", scope);
   buckets[*bucket_count].count = 0;
   return &buckets[(*bucket_count)++];
}

void mem_eval_support_case_add(mem_eval_support_case_t *scase, int64_t id)
{
   if (!scase || id <= 0 ||
       scase->n_relevant >= (int)(sizeof(scase->relevant_ids) / sizeof(scase->relevant_ids[0])))
      return;
   for (int i = 0; i < scase->n_relevant; i++)
      if (scase->relevant_ids[i] == id)
         return;
   scase->relevant_ids[scase->n_relevant++] = id;
}

int mem_eval_run_support_with_latency(mem_eval_support_case_t *cases, int n_cases,
                                      mem_eval_scores_t *out, mem_eval_latency_t *latency_out)
{
   if (!cases || !out || n_cases <= 0)
      return -1;

   memset(out, 0, sizeof(*out));
   out->n_cases = n_cases;
   if (latency_out)
      memset(latency_out, 0, sizeof(*latency_out));

   double total_mrr = 0.0, total_ndcg5 = 0.0, total_ndcg10 = 0.0;
   double total_recall5 = 0.0, total_recall10 = 0.0;
   double *latencies = latency_out ? calloc((size_t)n_cases, sizeof(double)) : NULL;

   for (int c = 0; c < n_cases; c++)
   {
      struct timespec ts0, ts1;
      if (latency_out)
         clock_gettime(CLOCK_MONOTONIC, &ts0);
      memory_t results[20];
      int n_results = memory_find_facts(cases[c].query, 20, results, 20);
      if (n_results < 0)
      {
         free(latencies);
         return -1;
      }
      if (latency_out)
      {
         clock_gettime(CLOCK_MONOTONIC, &ts1);
         latencies[c] = elapsed_ms(&ts0, &ts1);
      }

      int64_t retrieved[20];
      memset(retrieved, 0, sizeof(retrieved));
      for (int i = 0; i < n_results; i++)
         retrieved[i] = results[i].id;

      total_mrr += ir_mrr(retrieved, n_results, cases[c].relevant_ids, cases[c].n_relevant);
      total_ndcg5 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].relevant_ids, cases[c].n_relevant, 5);
      total_ndcg10 +=
          ir_ndcg_at_k(retrieved, n_results, cases[c].relevant_ids, cases[c].n_relevant, 10);
      total_recall5 +=
          ir_recall_at_k(retrieved, n_results, cases[c].relevant_ids, cases[c].n_relevant, 5);
      total_recall10 +=
          ir_recall_at_k(retrieved, n_results, cases[c].relevant_ids, cases[c].n_relevant, 10);
   }

   out->mrr = total_mrr / n_cases;
   out->ndcg_5 = total_ndcg5 / n_cases;
   out->ndcg_10 = total_ndcg10 / n_cases;
   out->recall_5 = total_recall5 / n_cases;
   out->recall_10 = total_recall10 / n_cases;
   if (latency_out)
      mem_eval_latency_finalize(latencies, n_cases, latency_out);
   free(latencies);
   return 0;
}

static int mem_eval_estimate_tokens(const char *text)
{
   size_t len = text ? strlen(text) : 0;
   return (int)((len + 3) / 4);
}

static void mem_eval_normalize_answer(const char *src, char *dst, size_t dst_len)
{
   if (!dst || dst_len == 0)
      return;
   dst[0] = '\0';
   if (!src || !src[0])
      return;

   size_t used = 0;
   int last_space = 1;
   for (const unsigned char *p = (const unsigned char *)src; *p && used + 1 < dst_len; p++)
   {
      if (isalnum(*p))
      {
         dst[used++] = (char)tolower(*p);
         last_space = 0;
      }
      else if (!last_space)
      {
         dst[used++] = ' ';
         last_space = 1;
      }
   }
   while (used > 0 && dst[used - 1] == ' ')
      used--;
   dst[used] = '\0';
}

static int mem_eval_answers_exact_match(const char *gold, const char *candidate)
{
   char norm_gold[512];
   char norm_candidate[512];
   mem_eval_normalize_answer(gold, norm_gold, sizeof(norm_gold));
   mem_eval_normalize_answer(candidate, norm_candidate, sizeof(norm_candidate));
   return norm_gold[0] && strcmp(norm_gold, norm_candidate) == 0;
}

static int mem_eval_parse_judge_score(const char *response, int *score_out)
{
   if (!response || !score_out)
      return -1;

   const char *p = response + strlen(response);
   const char *found = NULL;
   while (p >= response)
   {
      if (*p == '{' && strstr(p, "\"score\""))
      {
         found = p;
         break;
      }
      p--;
   }
   if (!found)
      return -1;

   cJSON *obj = cJSON_Parse(found);
   if (!obj)
      return -1;

   cJSON *score = cJSON_GetObjectItemCaseSensitive(obj, "score");
   if (cJSON_IsBool(score))
      *score_out = cJSON_IsTrue(score) ? 1 : 0;
   else if (cJSON_IsNumber(score))
      *score_out = score->valuedouble >= 0.5 ? 1 : 0;
   else
   {
      cJSON_Delete(obj);
      return -1;
   }

   cJSON_Delete(obj);
   return 0;
}

int mem_eval_score_case(const mem_eval_case_t *ecase, mem_eval_direct_trace_t *trace_out)
{
   if (!ecase || !trace_out)
      return -1;

   memset(trace_out, 0, sizeof(*trace_out));
   struct timespec ts0, ts1;
   clock_gettime(CLOCK_MONOTONIC, &ts0);
   memory_t results[20];
   int n_results = memory_find_facts(ecase->query, 20, results, 20);
   if (n_results < 0)
      return -1;
   clock_gettime(CLOCK_MONOTONIC, &ts1);

   int64_t retrieved[20];
   memset(retrieved, 0, sizeof(retrieved));
   trace_out->n_retrieved = n_results < 20 ? n_results : 20;
   for (int i = 0; i < trace_out->n_retrieved; i++)
   {
      retrieved[i] = results[i].id;
      trace_out->retrieved_ids[i] = results[i].id;
   }

   trace_out->latency_ms = elapsed_ms(&ts0, &ts1);
   trace_out->mrr = ir_mrr(retrieved, n_results, ecase->expected_ids, ecase->n_expected);
   trace_out->ndcg_5 =
       ir_ndcg_at_k(retrieved, n_results, ecase->expected_ids, ecase->n_expected, 5);
   trace_out->ndcg_10 =
       ir_ndcg_at_k(retrieved, n_results, ecase->expected_ids, ecase->n_expected, 10);
   trace_out->recall_5 =
       ir_recall_at_k(retrieved, n_results, ecase->expected_ids, ecase->n_expected, 5);
   trace_out->recall_10 =
       ir_recall_at_k(retrieved, n_results, ecase->expected_ids, ecase->n_expected, 10);
   return 0;
}

FILE *mem_eval_open_progress_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *fp = fopen(path, "w");
   if (!fp)
      return NULL;
   setvbuf(fp, NULL, _IOLBF, 0);
   return fp;
}

FILE *mem_eval_open_append_progress_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *fp = fopen(path, "a");
   if (!fp)
      return NULL;
   setvbuf(fp, NULL, _IOLBF, 0);
   return fp;
}

void mem_eval_append_direct_progress_row(FILE *fp, const char *dataset, const char *question_id,
                                         const char *label_key, const char *label_value,
                                         const char *question, const char *gold_answer,
                                         const mem_eval_direct_trace_t *trace)
{
   if (!fp || !dataset || !question_id || !label_key || !label_value || !question || !gold_answer ||
       !trace)
      return;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dataset", dataset);
   cJSON_AddStringToObject(root, "track", "direct");
   cJSON_AddStringToObject(root, "question_id", question_id);
   cJSON_AddStringToObject(root, label_key, label_value);
   cJSON_AddStringToObject(root, "question", question);
   cJSON_AddStringToObject(root, "gold_answer", gold_answer);
   cJSON_AddStringToObject(root, "verdict", trace->recall_5 > 0.0 ? "CORRECT" : "WRONG");
   cJSON_AddNumberToObject(root, "retrieval_latency_ms", trace->latency_ms);
   cJSON_AddNumberToObject(root, "mrr", trace->mrr);
   cJSON_AddNumberToObject(root, "ndcg_5", trace->ndcg_5);
   cJSON_AddNumberToObject(root, "ndcg_10", trace->ndcg_10);
   cJSON_AddNumberToObject(root, "recall_5", trace->recall_5);
   cJSON_AddNumberToObject(root, "recall_10", trace->recall_10);

   cJSON *retrieved = cJSON_AddArrayToObject(root, "retrieved_ids");
   for (int i = 0; i < trace->n_retrieved; i++)
      cJSON_AddItemToArray(retrieved, cJSON_CreateNumber((double)trace->retrieved_ids[i]));

   char *line = cJSON_PrintUnformatted(root);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      fflush(fp);
      free(line);
   }
   cJSON_Delete(root);
}

void mem_eval_append_miss_setup_progress_row(FILE *fp, const char *dataset, const char *unit_kind,
                                             int setup_completed, int setup_expected)
{
   if (!fp || !dataset)
      return;

   cJSON *root = cJSON_CreateObject();
   cJSON_AddStringToObject(root, "dataset", dataset);
   cJSON_AddStringToObject(root, "phase", "miss_report_setup");
   cJSON_AddStringToObject(root, "unit_kind", unit_kind ? unit_kind : "item");
   cJSON_AddNumberToObject(root, "setup_completed", setup_completed);
   cJSON_AddNumberToObject(root, "setup_expected", setup_expected);

   char *line = cJSON_PrintUnformatted(root);
   if (line)
   {
      fprintf(fp, "%s\n", line);
      fflush(fp);
      free(line);
   }
   cJSON_Delete(root);
}

static int mem_eval_build_retrieval_context(const char *query, int top_k, int token_budget,
                                            char *context_out, size_t context_len,
                                            int *retrieved_tokens_out)
{
   if (!query || !context_out || context_len == 0)
      return 0;

   if (top_k <= 0)
      top_k = 10;
   if (token_budget <= 0)
      token_budget = 2000;

   memory_t results[32];
   int fetch_limit = top_k * 4;
   if (fetch_limit < top_k)
      fetch_limit = top_k;
   if (fetch_limit > 32)
      fetch_limit = 32;
   int n_results = memory_find_facts(query, fetch_limit, results, 32);
   if (n_results < 0)
      return -1;
   size_t used = 0;
   int tokens = 0;
   int kept = 0;
   int per_item_token_cap = token_budget / 2;
   if (per_item_token_cap < 96)
      per_item_token_cap = token_budget;
   context_out[0] = '\0';

   for (int i = 0; i < n_results && kept < top_k; i++)
   {
      char line[2304];
      const char *content = results[i].content;
      char trimmed[1600];
      int max_chars = per_item_token_cap * 4;
      if (max_chars > (int)sizeof(trimmed) - 4)
         max_chars = (int)sizeof(trimmed) - 4;
      if ((int)strlen(content) > max_chars && per_item_token_cap < token_budget)
      {
         snprintf(trimmed, sizeof(trimmed), "%.*s...", max_chars, content);
         content = trimmed;
      }
      int line_len = snprintf(line, sizeof(line), "[%d] %s\n", kept + 1, content);
      if (line_len <= 0)
         continue;
      int line_tokens = mem_eval_estimate_tokens(line);
      if (line_tokens > per_item_token_cap && kept > 0)
         continue;
      if (kept > 0 && tokens + line_tokens > token_budget)
         continue;
      if (used + (size_t)line_len + 1 >= context_len)
         continue;
      memcpy(context_out + used, line, (size_t)line_len);
      used += (size_t)line_len;
      context_out[used] = '\0';
      tokens += line_tokens;
      kept++;
   }

   if (retrieved_tokens_out)
      *retrieved_tokens_out = tokens;
   return kept;
}

static int mem_eval_write_text_file(const char *path, const char *text)
{
   if (!path)
      return -1;

   FILE *fp = fopen(path, "w");
   if (!fp)
      return -1;
   if (text && text[0])
      fputs(text, fp);
   fclose(fp);
   return 0;
}

static int mem_eval_should_use_codex_exec(const agent_t *runner)
{
   if (!runner)
      return 0;
   return strcmp(runner->provider, "codex") == 0 || strcmp(runner->cli_cmd, "codex") == 0;
}

static int mem_eval_run_codex_exec(const agent_t *runner, const char *system_prompt,
                                   const char *user_prompt, agent_result_t *out)
{
   if (!runner || !out || !system_prompt || !user_prompt)
      return -1;

   memset(out, 0, sizeof(*out));
   snprintf(out->agent_name, sizeof(out->agent_name), "%s",
            runner->name[0] ? runner->name : "benchmark-codex");

   char prompt_path[] = "/tmp/aimee_eval_prompt_XXXXXX";
   char output_path[] = "/tmp/aimee_eval_output_XXXXXX";
   int prompt_fd = mkstemp(prompt_path);
   int output_fd = mkstemp(output_path);
   if (prompt_fd < 0 || output_fd < 0)
   {
      if (prompt_fd >= 0)
      {
         close(prompt_fd);
         unlink(prompt_path);
      }
      if (output_fd >= 0)
      {
         close(output_fd);
         unlink(output_path);
      }
      return -1;
   }
   close(prompt_fd);
   close(output_fd);

   char full_prompt[24576];
   snprintf(full_prompt, sizeof(full_prompt), "System instructions:\n%s\n\nUser request:\n%s\n",
            system_prompt, user_prompt);
   if (mem_eval_write_text_file(prompt_path, full_prompt) != 0)
   {
      unlink(prompt_path);
      unlink(output_path);
      return -1;
   }

   char cwd[MAX_PATH_LEN];
   cwd[0] = '\0';
   if (!getcwd(cwd, sizeof(cwd)))
      snprintf(cwd, sizeof(cwd), ".");

   char *esc_prompt = shell_escape(prompt_path);
   char *esc_output = shell_escape(output_path);
   char *esc_cwd = shell_escape(cwd);
   char *esc_model = runner->model[0] ? shell_escape(runner->model) : NULL;
   char model_arg[256];
   if (esc_model)
      snprintf(model_arg, sizeof(model_arg), " -m '%s'", esc_model);
   else
      model_arg[0] = '\0';

   char cmd[4096];
   snprintf(cmd, sizeof(cmd),
            "codex exec --skip-git-repo-check --sandbox read-only --color never -C '%s'%s "
            "--output-last-message '%s' - < '%s' 2>&1",
            esc_cwd, model_arg, esc_output, esc_prompt);

   int rc = 0;
   char *cli_output = run_cmd(cmd, &rc);
   char *response = slurp_file_eval(output_path);
   if (!response && cli_output && cli_output[0])
      response = strdup(cli_output);

   if (response && response[0] && rc == 0)
   {
      out->success = 1;
      out->response = response;
      out->prompt_tokens =
          mem_eval_estimate_tokens(system_prompt) + mem_eval_estimate_tokens(user_prompt);
      out->completion_tokens = mem_eval_estimate_tokens(response);
   }
   else
   {
      if (response)
         free(response);
      out->success = 0;
      snprintf(out->error, sizeof(out->error), "%s",
               (cli_output && cli_output[0]) ? cli_output : "codex exec failed");
   }

   free(cli_output);
   free(esc_prompt);
   free(esc_output);
   free(esc_cwd);
   free(esc_model);
   unlink(prompt_path);
   unlink(output_path);
   return out->success ? 0 : -1;
}

void mem_eval_insert_longmemeval_qa_session(const char *session_id, const char *date_str,
                                            cJSON *sess)
{
   if (!session_id || !sess || !cJSON_IsArray(sess))
      return;

   char session_content[2048];
   size_t used = 0;
   session_content[0] = '\0';
   if (date_str && date_str[0])
      used += (size_t)snprintf(session_content + used, sizeof(session_content) - used, "[%s] ",
                               date_str);

   int turn_index = 0;
   cJSON *turn = NULL;
   cJSON_ArrayForEach(turn, sess)
   {
      cJSON *role = cJSON_GetObjectItem(turn, "role");
      cJSON *turn_content = cJSON_GetObjectItem(turn, "content");
      const char *role_str =
          (role && cJSON_IsString(role) && role->valuestring[0]) ? role->valuestring : "turn";
      if (!turn_content || !cJSON_IsString(turn_content) || !turn_content->valuestring[0])
         continue;

      if (used < sizeof(session_content) - 1)
      {
         used += (size_t)snprintf(session_content + used, sizeof(session_content) - used, "%s: %s ",
                                  role_str, turn_content->valuestring);
      }

      char turn_key[512];
      char turn_text[2048];
      snprintf(turn_key, sizeof(turn_key), "longmemeval %s %s turn %d", role_str, session_id,
               turn_index + 1);
      if (date_str && date_str[0])
         snprintf(turn_text, sizeof(turn_text), "[%s] %s: %s", date_str, role_str,
                  turn_content->valuestring);
      else
         snprintf(turn_text, sizeof(turn_text), "%s: %s", role_str, turn_content->valuestring);
      (void)memory_insert(TIER_L2, KIND_FACT, turn_key, turn_text,
                          strcmp(role_str, "user") == 0 ? 0.98 : 0.9, "longmemeval-turn", NULL);

      if (strcmp(role_str, "user") == 0)
      {
         const char *scan = turn_content->valuestring;
         int sentence_index = 0;
         while (scan && *scan)
         {
            while (*scan == ' ' || *scan == '\t' || *scan == '\n')
               scan++;
            if (!*scan)
               break;
            const char *end = scan;
            while (*end && *end != '.' && *end != '!' && *end != '?')
               end++;
            while (*end == '.' || *end == '!' || *end == '?')
               end++;

            size_t seg_len = (size_t)(end - scan);
            if (seg_len >= 24)
            {
               char sentence[768];
               if (seg_len >= sizeof(sentence))
                  seg_len = sizeof(sentence) - 1;
               memcpy(sentence, scan, seg_len);
               sentence[seg_len] = '\0';

               if (strcasestr(sentence, " i ") || strcasestr(sentence, " i'm") ||
                   strcasestr(sentence, " i've") || strcasestr(sentence, " my ") ||
                   strcasestr(sentence, " me "))
               {
                  char fact_key[512];
                  char fact_text[1024];
                  snprintf(fact_key, sizeof(fact_key), "longmemeval fact %s turn %d sentence %d",
                           session_id, turn_index + 1, sentence_index + 1);
                  if (date_str && date_str[0])
                     snprintf(fact_text, sizeof(fact_text), "[%s] user: %s", date_str, sentence);
                  else
                     snprintf(fact_text, sizeof(fact_text), "user: %s", sentence);
                  (void)memory_insert(TIER_L2, KIND_FACT, fact_key, fact_text, 1.0,
                                      "longmemeval-fact", NULL);
                  sentence_index++;
               }
            }
            scan = end;
         }
      }
      turn_index++;
   }

   char session_key[512];
   snprintf(session_key, sizeof(session_key), "longmemeval session %s", session_id);
   (void)memory_insert(TIER_L2, KIND_FACT, session_key, session_content, 0.9, "longmemeval", NULL);
}

int mem_eval_run_qa_with_latency(agent_config_t *cfg, mem_eval_qa_case_t *cases, int n_cases,
                                 int top_k, int token_budget, mem_eval_qa_scores_t *out,
                                 mem_eval_latency_t *latency_out)
{
   if (!cfg || !cases || n_cases <= 0 || !out)
      return -1;
   memset(out, 0, sizeof(*out));
   out->n_cases = n_cases;
   if (latency_out)
      memset(latency_out, 0, sizeof(*latency_out));
   double total_accuracy = 0.0;
   double total_exact = 0.0;
   double *latencies = latency_out ? calloc((size_t)n_cases, sizeof(double)) : NULL;
   static const char *answer_system =
       "Answer benchmark questions using only the provided memory context. "
       "If the context is insufficient, answer Unknown. Respond with only the answer.";
   static const char *judge_system =
       "You are grading benchmark answers. Compare the candidate answer to the gold answer for "
       "semantic equivalence. Ignore wording differences. Be strict about factual disagreement. "
       "Return JSON only: {\"score\":1} for correct or {\"score\":0} for incorrect.";
   agent_t *runner = agent_route(cfg, "execute");
   if (!runner)
      return -1;
   for (int i = 0; i < n_cases; i++)
   {
      char context[16384];
      char answer_prompt[20000];
      char judge_prompt[4096];
      int retrieved_tokens = 0;
      int exact = 0;
      int judged_correct = 0;
      int has_citation = 0;
      (void)mem_eval_build_retrieval_context(cases[i].question, top_k, token_budget, context,
                                             sizeof(context), &retrieved_tokens);
      out->avg_retrieved_tokens += retrieved_tokens;
      snprintf(answer_prompt, sizeof(answer_prompt),
               "Question: %s\n\nRetrieved memory context:\n%s\nFinal answer:", cases[i].question,
               context[0] ? context : "[no retrieved context]");
      struct timespec ts0, ts1;
      if (latency_out)
         clock_gettime(CLOCK_MONOTONIC, &ts0);
      agent_result_t answer_result;
      memset(&answer_result, 0, sizeof(answer_result));
      int answer_rc;
      if (mem_eval_should_use_codex_exec(runner))
         answer_rc = mem_eval_run_codex_exec(runner, answer_system, answer_prompt, &answer_result);
      else if (strcmp(runner->backend, AGENT_BACKEND_TMUX_CLI) == 0)
         answer_rc = agent_execute_with_tools(runner, &cfg->network, answer_system, answer_prompt,
                                              256, 0.3, &answer_result);
      else
         answer_rc = agent_run(cfg, "execute", answer_system, answer_prompt, 256, &answer_result);
      if (answer_rc == 0 && answer_result.success && answer_result.response)
      {
         int answer_prompt_tokens = answer_result.prompt_tokens > 0
                                        ? answer_result.prompt_tokens
                                        : mem_eval_estimate_tokens(answer_system) +
                                              mem_eval_estimate_tokens(answer_prompt);
         int answer_completion_tokens = answer_result.completion_tokens > 0
                                            ? answer_result.completion_tokens
                                            : mem_eval_estimate_tokens(answer_result.response);
         out->answered_cases++;
         out->total_answer_prompt_tokens += answer_prompt_tokens;
         out->total_answer_completion_tokens += answer_completion_tokens;
         has_citation = memory_citation_gate_check(answer_result.response);
         out->cited_answers += has_citation ? 1 : 0;
         out->uncited_answers += has_citation ? 0 : 1;
         out->low_confidence_answers +=
             strncmp(answer_result.response, "## Retrieval Confidence: LOW", 28) == 0;
         exact = mem_eval_answers_exact_match(cases[i].answer, answer_result.response);
         snprintf(judge_prompt, sizeof(judge_prompt),
                  "Question: %s\nGold answer: %s\nCandidate answer: %s\n\nReturn JSON only.",
                  cases[i].question, cases[i].answer, answer_result.response);
         agent_result_t judge_result;
         memset(&judge_result, 0, sizeof(judge_result));
         int judge_rc;
         if (mem_eval_should_use_codex_exec(runner))
            judge_rc = mem_eval_run_codex_exec(runner, judge_system, judge_prompt, &judge_result);
         else if (strcmp(runner->backend, AGENT_BACKEND_TMUX_CLI) == 0)
            judge_rc = agent_execute_with_tools(runner, &cfg->network, judge_system, judge_prompt,
                                                64, 0.0, &judge_result);
         else
            judge_rc = agent_run(cfg, "execute", judge_system, judge_prompt, 64, &judge_result);
         if (judge_rc == 0 && judge_result.success && judge_result.response)
         {
            int judge_prompt_tokens = judge_result.prompt_tokens > 0
                                          ? judge_result.prompt_tokens
                                          : mem_eval_estimate_tokens(judge_system) +
                                                mem_eval_estimate_tokens(judge_prompt);
            int judge_completion_tokens = judge_result.completion_tokens > 0
                                              ? judge_result.completion_tokens
                                              : mem_eval_estimate_tokens(judge_result.response);
            out->judged_cases++;
            out->total_judge_prompt_tokens += judge_prompt_tokens;
            out->total_judge_completion_tokens += judge_completion_tokens;
            if (mem_eval_parse_judge_score(judge_result.response, &judged_correct) != 0)
               judged_correct = exact;
         }
         else
         {
            judged_correct = exact;
         }
         free(judge_result.response);
      }
      if (latency_out)
      {
         clock_gettime(CLOCK_MONOTONIC, &ts1);
         latencies[i] = elapsed_ms(&ts0, &ts1);
      }
      total_exact += exact ? 1.0 : 0.0;
      total_accuracy += judged_correct ? 1.0 : 0.0;
      free(answer_result.response);
   }

   out->accuracy = total_accuracy / n_cases;
   out->exact_match = total_exact / n_cases;
   out->avg_retrieved_tokens /= n_cases;
   out->citation_coverage =
       out->answered_cases > 0 ? (double)out->cited_answers / out->answered_cases : 0.0;
   out->citation_miss_rate =
       out->answered_cases > 0 ? (double)out->uncited_answers / out->answered_cases : 0.0;
   out->hallucination_rate = 1.0 - out->accuracy;
   if (out->answered_cases <= 0)
      return -1;
   if (latency_out)
      mem_eval_latency_finalize(latencies, n_cases, latency_out);
   free(latencies);
   return 0;
}

int mem_eval_trace_qa_case(agent_config_t *cfg, const mem_eval_qa_case_t *qcase, int top_k,
                           int token_budget, mem_eval_qa_trace_t *trace_out)
{
   if (!cfg || !qcase || !trace_out)
      return -1;

   memset(trace_out, 0, sizeof(*trace_out));
   snprintf(trace_out->question, sizeof(trace_out->question), "%s", qcase->question);
   snprintf(trace_out->gold_answer, sizeof(trace_out->gold_answer), "%s", qcase->answer);

   static const char *answer_system =
       "Answer benchmark questions using only the provided memory context. "
       "If the context is insufficient, answer Unknown. Respond with only the answer.";
   static const char *judge_system =
       "You are grading benchmark answers. Compare the candidate answer to the gold answer for "
       "semantic equivalence. Ignore wording differences. Be strict about factual disagreement. "
       "Return JSON only: {\"score\":1} for correct or {\"score\":0} for incorrect.";

   agent_t *runner = agent_route(cfg, "execute");
   if (!runner)
      return -1;

   char answer_prompt[20000];
   char judge_prompt[4096];
   (void)mem_eval_build_retrieval_context(
       qcase->question, top_k, token_budget, trace_out->retrieved_context,
       sizeof(trace_out->retrieved_context), &trace_out->retrieved_tokens);

   snprintf(answer_prompt, sizeof(answer_prompt),
            "Question: %s\n\nRetrieved memory context:\n%s\nFinal answer:", qcase->question,
            trace_out->retrieved_context[0] ? trace_out->retrieved_context
                                            : "[no retrieved context]");

   agent_result_t answer_result;
   memset(&answer_result, 0, sizeof(answer_result));
   int answer_rc =
       mem_eval_should_use_codex_exec(runner)
           ? mem_eval_run_codex_exec(runner, answer_system, answer_prompt, &answer_result)
           : (strcmp(runner->backend, AGENT_BACKEND_TMUX_CLI) == 0
                  ? agent_execute_with_tools(runner, &cfg->network, answer_system, answer_prompt,
                                             256, 0.3, &answer_result)
                  : agent_run(cfg, "execute", answer_system, answer_prompt, 256, &answer_result));
   if (answer_rc == 0 && answer_result.success && answer_result.response)
   {
      trace_out->answered = 1;
      snprintf(trace_out->model_answer, sizeof(trace_out->model_answer), "%s",
               answer_result.response);
      trace_out->exact = mem_eval_answers_exact_match(qcase->answer, answer_result.response);

      snprintf(judge_prompt, sizeof(judge_prompt),
               "Question: %s\nGold answer: %s\nCandidate answer: %s\n\nReturn JSON only.",
               qcase->question, qcase->answer, answer_result.response);

      agent_result_t judge_result;
      memset(&judge_result, 0, sizeof(judge_result));
      int judge_rc =
          mem_eval_should_use_codex_exec(runner)
              ? mem_eval_run_codex_exec(runner, judge_system, judge_prompt, &judge_result)
              : (strcmp(runner->backend, AGENT_BACKEND_TMUX_CLI) == 0
                     ? agent_execute_with_tools(runner, &cfg->network, judge_system, judge_prompt,
                                                64, 0.0, &judge_result)
                     : agent_run(cfg, "execute", judge_system, judge_prompt, 64, &judge_result));
      if (judge_rc == 0 && judge_result.success && judge_result.response)
      {
         trace_out->judged = 1;
         snprintf(trace_out->judge_response, sizeof(trace_out->judge_response), "%s",
                  judge_result.response);
         if (mem_eval_parse_judge_score(judge_result.response, &trace_out->judged_correct) != 0)
            trace_out->judged_correct = trace_out->exact;
      }
      free(judge_result.response);
   }

   free(answer_result.response);
   return 0;
}

void mem_eval_print_qa_failure(FILE *fp, const mem_eval_qa_trace_t *trace)
{
   if (!fp || !trace)
      return;
   fprintf(fp, "FAIL question=%s\n", trace->question);
   fprintf(fp, "  gold=%s\n", trace->gold_answer);
   fprintf(fp, "  answer=%s\n", trace->answered ? trace->model_answer : "(no answer)");
   fprintf(fp, "  exact=%d judged=%d judged_correct=%d retrieved_tokens=%d\n", trace->exact,
           trace->judged, trace->judged_correct, trace->retrieved_tokens);
   if (trace->judged)
      fprintf(fp, "  judge=%s\n", trace->judge_response);
   fprintf(fp, "  context=%s\n",
           trace->retrieved_context[0] ? trace->retrieved_context : "[no retrieved context]");
}

int mem_eval_load_benchmark_agent_config(agent_config_t *cfg)
{
   if (!cfg)
      return -1;

   memset(cfg, 0, sizeof(*cfg));
   if (agent_load_config(cfg) == 0 && cfg->agent_count > 0)
   {
      agent_t *runner = agent_route(cfg, "execute");
      if (runner)
         return 0;
   }

   memset(cfg, 0, sizeof(*cfg));
   agent_t *ag = &cfg->agents[0];
   memset(ag, 0, sizeof(*ag));
   snprintf(ag->name, sizeof(ag->name), "benchmark-codex");
   snprintf(ag->provider, sizeof(ag->provider), "codex");
   snprintf(ag->backend, sizeof(ag->backend), "%s", AGENT_BACKEND_TMUX_CLI);
   snprintf(ag->cli_cmd, sizeof(ag->cli_cmd), "codex");
   snprintf(ag->roles[ag->role_count++], sizeof(ag->roles[0]), "execute");
   snprintf(ag->exec_roles[ag->exec_role_count++], sizeof(ag->exec_roles[0]), "execute");
   ag->enabled = 1;
   ag->tools_enabled = 0;
   ag->timeout_ms = 180000;
   ag->max_tokens = 1024;
   ag->max_turns = 1;
   ag->cost_tier = 1;
   ag->session_reuse = 0;

   cfg->agent_count = 1;
   snprintf(cfg->default_agent, sizeof(cfg->default_agent), "%s", ag->name);
   return 0;
}

int mem_eval_load_corpus(const char *corpus_path, mem_eval_case_t *cases, int max_cases)
{
   if (!corpus_path || !cases || max_cases <= 0)
      return -1;

   char *raw = slurp_file_eval(corpus_path);
   if (!raw)
   {
      fprintf(stderr, "mem_eval_load_corpus: cannot read %s\n", corpus_path);
      return -1;
   }

   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root)
   {
      fprintf(stderr, "mem_eval_load_corpus: JSON parse error in %s\n", corpus_path);
      return -1;
   }

   /* Open a fresh in-memory scratch DB; the handle is held in module
    * state and torn down by mem_eval_close_temp_db(). */
   if (mem_eval_open_temp_db() != 0)
   {
      cJSON_Delete(root);
      fprintf(stderr, "mem_eval_load_corpus: cannot open in-memory DB\n");
      return -1;
   }

   /* Insert fixtures and build fid → db_id map. */
   cJSON *fixtures = cJSON_GetObjectItem(root, "fixtures");
   int n_fixtures = fixtures ? cJSON_GetArraySize(fixtures) : 0;
   if (n_fixtures <= 0 || n_fixtures > MEM_CORPUS_MAX_FIXTURES)
   {
      fprintf(stderr, "mem_eval_load_corpus: expected 1-%d fixtures, got %d\n",
              MEM_CORPUS_MAX_FIXTURES, n_fixtures);
      cJSON_Delete(root);
      mem_eval_close_temp_db();
      return -1;
   }

   corpus_fid_entry_t *fid_map = calloc((size_t)n_fixtures, sizeof(corpus_fid_entry_t));
   if (!fid_map)
   {
      cJSON_Delete(root);
      mem_eval_close_temp_db();
      return -1;
   }

   int n_inserted = 0;
   cJSON *fix = NULL;
   cJSON_ArrayForEach(fix, fixtures)
   {
      cJSON *j_fid = cJSON_GetObjectItem(fix, "fid");
      cJSON *j_tier = cJSON_GetObjectItem(fix, "tier");
      cJSON *j_kind = cJSON_GetObjectItem(fix, "kind");
      cJSON *j_key = cJSON_GetObjectItem(fix, "key");
      cJSON *j_content = cJSON_GetObjectItem(fix, "content");

      if (!j_fid || !cJSON_IsString(j_fid) || !j_tier || !cJSON_IsString(j_tier) || !j_kind ||
          !cJSON_IsString(j_kind) || !j_key || !cJSON_IsString(j_key) || !j_content ||
          !cJSON_IsString(j_content))
      {
         fprintf(stderr, "mem_eval_load_corpus: malformed fixture at index %d\n", n_inserted);
         free(fid_map);
         cJSON_Delete(root);
         mem_eval_close_temp_db();
         return -1;
      }

      memory_t m;
      int rc = memory_insert(j_tier->valuestring, j_kind->valuestring, j_key->valuestring,
                             j_content->valuestring, 0.9, "corpus", &m);
      if (rc != 0)
      {
         fprintf(stderr, "mem_eval_load_corpus: memory_insert failed for fixture %s\n",
                 j_fid->valuestring);
         free(fid_map);
         cJSON_Delete(root);
         mem_eval_close_temp_db();
         return -1;
      }

      {
         config_t embed_cfg;
         config_load(&embed_cfg);
         const char *embed_cmd = config_embedding_command(&embed_cfg, NULL);
         if (memory_embed(m.id, embed_cmd) != 0)
         {
            fprintf(stderr, "mem_eval_load_corpus: memory_embed failed for fixture %s\n",
                    j_fid->valuestring);
            free(fid_map);
            cJSON_Delete(root);
            mem_eval_close_temp_db();
            return -1;
         }
      }

      snprintf(fid_map[n_inserted].fid, CORPUS_FID_LEN, "%s", j_fid->valuestring);
      fid_map[n_inserted].db_id = m.id;
      n_inserted++;
   }

   /* Build eval cases from the "cases" array. */
   cJSON *cases_arr = cJSON_GetObjectItem(root, "cases");
   int n_cases_json = cases_arr ? cJSON_GetArraySize(cases_arr) : 0;
   if (n_cases_json <= 0)
   {
      fprintf(stderr, "mem_eval_load_corpus: no cases found in %s\n", corpus_path);
      free(fid_map);
      cJSON_Delete(root);
      mem_eval_close_temp_db();
      return -1;
   }

   int n_cases_loaded = 0;
   cJSON *cas = NULL;
   cJSON_ArrayForEach(cas, cases_arr)
   {
      if (n_cases_loaded >= max_cases)
         break;

      cJSON *j_query = cJSON_GetObjectItem(cas, "query");
      cJSON *j_expected = cJSON_GetObjectItem(cas, "expected");

      if (!j_query || !cJSON_IsString(j_query) || !j_expected || !cJSON_IsArray(j_expected))
         continue;

      mem_eval_case_t *c = &cases[n_cases_loaded];
      memset(c, 0, sizeof(*c));
      snprintf(c->query, sizeof(c->query), "%s", j_query->valuestring);

      int n_exp = cJSON_GetArraySize(j_expected);
      for (int ei = 0; ei < n_exp && ei < 20; ei++)
      {
         cJSON *j_fid = cJSON_GetArrayItem(j_expected, ei);
         if (!j_fid || !cJSON_IsString(j_fid))
            continue;
         int64_t db_id = fid_lookup(fid_map, n_inserted, j_fid->valuestring);
         if (db_id > 0)
         {
            c->expected_ids[c->n_expected++] = db_id;
         }
      }

      if (c->n_expected > 0)
         n_cases_loaded++;
   }

   free(fid_map);
   cJSON_Delete(root);

   if (n_cases_loaded == 0)
   {
      fprintf(stderr, "mem_eval_load_corpus: no valid cases loaded from %s\n", corpus_path);
      mem_eval_close_temp_db();
      return -1;
   }

   return n_cases_loaded;
}

/* Load the code-vector-graph production corpus
 * (benchmarks/code-vector-graph/production-corpus.json): queries whose
 * `expected_ids` are already-resolved *live* DB2 memory ids. Unlike
 * mem_eval_load_corpus this opens NO temp/scratch DB — the cases are scored
 * against whatever store the process is connected to (the live DB2), which is
 * what the graph-code fusion ablation needs. Every well-formed query is loaded
 * (including ones whose expected_ids are still empty, so latency and the
 * harness can run before labelling — those simply score 0 recall). Returns the
 * number of cases loaded, or -1 on read/parse error. */
int mem_eval_load_production_corpus(const char *corpus_path, mem_eval_case_t *cases, int max_cases)
{
   if (!corpus_path || !cases || max_cases <= 0)
      return -1;

   char *raw = slurp_file_eval(corpus_path);
   if (!raw)
   {
      fprintf(stderr, "mem_eval_load_production_corpus: cannot read %s\n", corpus_path);
      return -1;
   }
   cJSON *root = cJSON_Parse(raw);
   free(raw);
   if (!root)
   {
      fprintf(stderr, "mem_eval_load_production_corpus: JSON parse error in %s\n", corpus_path);
      return -1;
   }

   cJSON *queries = cJSON_GetObjectItem(root, "queries");
   if (!cJSON_IsArray(queries))
   {
      cJSON_Delete(root);
      fprintf(stderr, "mem_eval_load_production_corpus: missing queries array in %s\n",
              corpus_path);
      return -1;
   }

   int n = 0;
   cJSON *q = NULL;
   cJSON_ArrayForEach(q, queries)
   {
      if (n >= max_cases)
         break;
      cJSON *jq = cJSON_GetObjectItem(q, "query");
      if (!jq || !cJSON_IsString(jq) || !jq->valuestring[0])
         continue;
      mem_eval_case_t *c = &cases[n];
      memset(c, 0, sizeof(*c));
      snprintf(c->query, sizeof(c->query), "%s", jq->valuestring);
      cJSON *jexp = cJSON_GetObjectItem(q, "expected_ids");
      if (cJSON_IsArray(jexp))
      {
         int ne = cJSON_GetArraySize(jexp);
         for (int ei = 0; ei < ne && c->n_expected < 20; ei++)
         {
            cJSON *jid = cJSON_GetArrayItem(jexp, ei);
            if (cJSON_IsNumber(jid))
               c->expected_ids[c->n_expected++] = (int64_t)jid->valuedouble;
         }
      }
      n++;
   }

   cJSON_Delete(root);
   return n;
}

int mem_eval_fusion_arm_resolve(const char *matrix_path, const char *arm, char *state_out,
                                size_t state_len, int *utility_out, int *projection_out)
{
   if (!matrix_path || !arm || !arm[0])
      return -1;
   char *buf = slurp_file_eval(matrix_path);
   if (!buf)
      return -1;
   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return -1;

   int found = -1;
   cJSON *arms = cJSON_GetObjectItem(root, "arms");
   cJSON *a = NULL;
   cJSON_ArrayForEach(a, arms)
   {
      cJSON *name = cJSON_GetObjectItem(a, "name");
      if (!cJSON_IsString(name) || strcmp(name->valuestring, arm) != 0)
         continue;
      cJSON *cfg = cJSON_GetObjectItem(a, "config");
      if (cJSON_IsObject(cfg))
      {
         cJSON *st = cJSON_GetObjectItem(cfg, "memory.recall.graph_code_fusion_state");
         if (cJSON_IsString(st) && state_out && state_len > 0)
            snprintf(state_out, state_len, "%s", st->valuestring);
         cJSON *us = cJSON_GetObjectItem(cfg, "memory.graph.utility_scoring_enabled");
         if (cJSON_IsNumber(us) && utility_out)
            *utility_out = us->valuedouble != 0;
         cJSON *cp = cJSON_GetObjectItem(cfg, "memory.graph.code_projection_enabled");
         if (cJSON_IsNumber(cp) && projection_out)
            *projection_out = cp->valuedouble != 0;
      }
      found = 0;
      break;
   }
   cJSON_Delete(root);
   return found;
}

/* Regression-baseline I/O (mem_eval_load_baseline / mem_eval_check_regression /
 * mem_eval_save_baseline) lives in agent_eval_baseline.c. */

int append_latency_sample(double **samples, int *count, int *cap, double value)
{
   if (!samples || !count || !cap)
      return -1;
   if (*count >= *cap)
   {
      int new_cap = (*cap == 0) ? 128 : (*cap * 2);
      double *grown = realloc(*samples, (size_t)new_cap * sizeof(double));
      if (!grown)
         return -1;
      *samples = grown;
      *cap = new_cap;
   }
   (*samples)[(*count)++] = value;
   return 0;
}

static int is_stopword_token(const char *tok)
{
   static const char *words[] = {
       "a",    "an",   "and",  "are",   "as",        "at",   "be",    "did",   "do",
       "does", "for",  "from", "go",    "had",       "has",  "have",  "he",    "her",
       "him",  "his",  "how",  "i",     "in",        "is",   "it",    "its",   "me",
       "of",   "on",   "or",   "she",   "that",      "the",  "their", "them",  "there",
       "they", "this", "to",   "was",   "were",      "what", "when",  "where", "which",
       "who",  "why",  "with", "would", "yesterday", NULL};
   for (int i = 0; words[i]; i++)
      if (strcmp(tok, words[i]) == 0)
         return 1;
   return 0;
}

void mem_eval_normalize_question(const char *question, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!question || !question[0])
      return;

   char token[128];
   size_t tlen = 0;
   size_t used = 0;
   for (const unsigned char *p = (const unsigned char *)question;; p++)
   {
      unsigned char ch = *p;
      int is_word =
          (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
      if (is_word && tlen + 1 < sizeof(token))
      {
         token[tlen++] = (char)tolower(ch);
         continue;
      }
      if (tlen > 0)
      {
         token[tlen] = '\0';
         if (!is_stopword_token(token))
         {
            used += (size_t)snprintf(out + used, out_len - used, used ? " %s" : "%s", token);
            if (used >= out_len - 1)
               break;
         }
         tlen = 0;
      }
      if (ch == '\0')
         break;
   }

   if (out[0] == '\0')
      snprintf(out, out_len, "%s", question);
}

/* Per-call isolated scratch DB2 store for bench scoring. */

void mem_eval_close_temp_db(void)
{
   db2_eval_close_temp_store();
}

int mem_eval_open_temp_db(void)
{
   if (db2_eval_open_temp_store() != 0)
   {
      fprintf(stderr, "mem_eval_open_temp_db: open/schema apply failed\n");
      return -1;
   }
   return 0;
}
