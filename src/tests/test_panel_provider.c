#include "aimee.h"

#include <aimee/delegates/panel_provider.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic int release_count;
static _Atomic int run_mode;
static const aimee_request_t *expected_request;

static int mock_aggregate(agent_config_t *agents, const ensemble_panel_t *panel,
                          const aimee_request_t *request, aimee_panel_aggregate_result_t *out)
{
   assert(!expected_request || request == expected_request);
   (void)agents;
   (void)panel;
   snprintf(out->response, sizeof out->response, "%s", request->messages[0].blocks[0].text);
   out->success = 1;
   return 0;
}

static int mock_run(agent_config_t *agents, const ensemble_panel_t *panel,
                    const aimee_request_t *request, const aimee_panel_options_t *options,
                    aimee_panel_result_t *out)
{
   assert(!expected_request || request == expected_request);
   (void)agents;
   (void)panel;
   (void)options;
   int mode = atomic_load_explicit(&run_mode, memory_order_relaxed);
   if (mode == 2)
      return 0; /* inconsistent provider: success without a result */
   out->artifact = strdup(request->messages[0].blocks[0].text);
   return mode == 1 ? -1 : 0; /* mode 1 allocates and then fails */
}

static void mock_release(aimee_panel_result_t *result)
{
   atomic_fetch_add_explicit(&release_count, 1, memory_order_relaxed);
   free(result->artifact);
   result->artifact = NULL;
}

static const aimee_panel_provider_t provider = {
    .aggregate = mock_aggregate,
    .run = mock_run,
    .release = mock_release,
};

static const aimee_panel_provider_t other_provider = {
    .aggregate = mock_aggregate,
    .run = mock_run,
    .release = mock_release,
};

static int fixture_panel_run(agent_config_t *agents, const ensemble_panel_t *panel,
                             const char *text, const aimee_panel_options_t *options,
                             aimee_panel_result_t *out)
{
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   return aimee_panel_run(agents, panel, &request, options, out);
}

static int fixture_panel_aggregate(agent_config_t *agents, const ensemble_panel_t *panel,
                                   const char *text, aimee_panel_aggregate_result_t *out)
{
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)text};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   return aimee_panel_aggregate(agents, panel, &request, out);
}

typedef struct
{
   agent_config_t *agents;
   ensemble_panel_t *panel;
   int iterations;
} worker_ctx_t;

static void *run_and_release_worker(void *arg)
{
   worker_ctx_t *ctx = arg;
   for (int i = 0; i < ctx->iterations; i++)
   {
      aimee_panel_result_t result;
      assert(fixture_panel_run(ctx->agents, ctx->panel, "concurrent", NULL, &result) ==
             AIMEE_PANEL_PROVIDER_OK);
      aimee_panel_result_release(&result);
   }
   return NULL;
}

int main(void)
{
   assert(AIMEE_PANEL_DRAFT == 0);
   assert(AIMEE_PANEL_REVIEW == 1);
   assert(AIMEE_PANEL_PARALLEL == 0);
   assert(AIMEE_PANEL_SEQUENTIAL == 1);

   agent_config_t agents = {0};
   ensemble_panel_t panel = {0};
   aimee_panel_result_t result;
   memset(&result, 0x7f, sizeof result);

   assert(!aimee_panel_provider_available());
   assert(fixture_panel_run(&agents, &panel, "task", NULL, &result) ==
          AIMEE_PANEL_PROVIDER_UNAVAILABLE);
   assert(result.artifact == NULL);
   assert(aimee_panel_provider_register(NULL) == AIMEE_PANEL_PROVIDER_INVALID);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(aimee_panel_provider_register(&other_provider) == AIMEE_PANEL_PROVIDER_BUSY);

   aimee_panel_aggregate_result_t aggregate;
   assert(fixture_panel_aggregate(&agents, &panel, "answer", &aggregate) ==
          AIMEE_PANEL_PROVIDER_OK);
   assert(aggregate.success == 1);
   assert(strcmp(aggregate.response, "answer") == 0);

   enum
   {
      WORKER_COUNT = 8,
      ITERATIONS_PER_WORKER = 100
   };
   pthread_t workers[WORKER_COUNT];
   worker_ctx_t worker_ctx = {
       .agents = &agents, .panel = &panel, .iterations = ITERATIONS_PER_WORKER};
   atomic_store_explicit(&release_count, 0, memory_order_relaxed);
   atomic_store_explicit(&run_mode, 0, memory_order_relaxed);
   for (int i = 0; i < WORKER_COUNT; i++)
      assert(pthread_create(&workers[i], NULL, run_and_release_worker, &worker_ctx) == 0);
   for (int i = 0; i < WORKER_COUNT; i++)
      assert(pthread_join(workers[i], NULL) == 0);
   assert(atomic_load_explicit(&release_count, memory_order_relaxed) ==
          WORKER_COUNT * ITERATIONS_PER_WORKER);
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);

   atomic_store_explicit(&release_count, 0, memory_order_relaxed);
   atomic_store_explicit(&run_mode, 0, memory_order_relaxed);
   assert(fixture_panel_run(&agents, &panel, "artifact", NULL, &result) == AIMEE_PANEL_PROVIDER_OK);
   assert(strcmp(result.artifact, "artifact") == 0);
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_BUSY);
   aimee_panel_result_release(&result);
   aimee_panel_result_release(&result);
   assert(atomic_load_explicit(&release_count, memory_order_relaxed) == 1);

   atomic_store_explicit(&run_mode, 1, memory_order_relaxed);
   assert(fixture_panel_run(&agents, &panel, "failed", NULL, &result) ==
          AIMEE_PANEL_PROVIDER_ERROR);
   assert(result.artifact == NULL);
   assert(atomic_load_explicit(&release_count, memory_order_relaxed) == 2);

   atomic_store_explicit(&run_mode, 2, memory_order_relaxed);
   assert(fixture_panel_run(&agents, &panel, "empty", NULL, &result) == AIMEE_PANEL_PROVIDER_ERROR);
   assert(result.artifact == NULL);
   assert(atomic_load_explicit(&release_count, memory_order_relaxed) == 2);

   assert(aimee_panel_provider_unregister(&other_provider) == AIMEE_PANEL_PROVIDER_INVALID);
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(!aimee_panel_provider_available());
   assert(fixture_panel_aggregate(&agents, &panel, "answer", &aggregate) ==
          AIMEE_PANEL_PROVIDER_UNAVAILABLE);
   assert(aggregate.response[0] == '\0');

   atomic_store_explicit(&run_mode, 0, memory_order_relaxed);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);
   aimee_block_t blocks[] = {{.type = AIMEE_BLK_TEXT, .text = "canonical task"},
                             {.type = AIMEE_BLK_IMAGE,
                              .media_type = "image/png",
                              .media_ref = "https://fixture.invalid/a.png"}};
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 2};
   aimee_request_t input = {
       .messages = &message, .n_messages = 1, .has_temperature = 1, .temperature = 0.0};
   expected_request = &input;
   assert(aimee_panel_run(&agents, &panel, &input, NULL, &result) == AIMEE_PANEL_PROVIDER_OK);
   aimee_panel_result_release(&result);
   assert(aimee_panel_aggregate(&agents, &panel, &input, &aggregate) == AIMEE_PANEL_PROVIDER_OK);
   input.n_messages = 0;
   assert(aimee_panel_run(&agents, &panel, &input, NULL, &result) == AIMEE_PANEL_PROVIDER_INVALID);
   assert(aimee_panel_aggregate(&agents, &panel, &input, &aggregate) ==
          AIMEE_PANEL_PROVIDER_INVALID);
   expected_request = NULL;
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_OK);

   puts("panel_provider tests: ok");
   return 0;
}
