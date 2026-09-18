/* Real Go context assembly behind the remaining benchmark host connection. */
#include "aimee.h"
#include "modules/benchmarks/agent_eval_internal.h"
#include "modules/memory/memory_bus_context.h"
#include "json_fluent.h"
#include "support/module_runtime_fixture.h"
#include <assert.h>

static int unavailable;
static const char *malformed;
void memory_bus_read_context(db2_memory_scope_context_t *context)
{
   memset(context, 0, sizeof(*context));
   context->active = 1;
   snprintf(context->project, sizeof(context->project), "bench-project");
}
int aimee_module_commands_dispatch_internal(const char *method, const cJSON *args, cJSON **result)
{
   assert(!strcmp(method, "memory.runtime"));
   assert(!strcmp(jo_cstr(args, "operation"), "benchmark-context"));
   assert(!strcmp(jo_cstr(args, "project"), "bench-project"));
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
   puts("benchmark context: real Go assembly, full UTF-8, budgets and failures passed");
   return 0;
}
