#include "aimee.h"

#include <aimee/delegates/panel_provider.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int release_count;
static int run_mode;

static int mock_aggregate(agent_config_t *agents, const config_t *cfg, const char *prompt,
                          aimee_panel_aggregate_result_t *out)
{
   (void)agents;
   (void)cfg;
   snprintf(out->response, sizeof out->response, "%s", prompt);
   out->success = 1;
   return 0;
}

static int mock_run(agent_config_t *agents, const config_t *cfg, const char *task,
                    const aimee_panel_options_t *options, aimee_panel_result_t *out)
{
   (void)agents;
   (void)cfg;
   (void)options;
   if (run_mode == 2)
      return 0; /* inconsistent provider: success without a result */
   out->artifact = strdup(task);
   return run_mode == 1 ? -1 : 0; /* mode 1 allocates and then fails */
}

static void mock_release(aimee_panel_result_t *result)
{
   release_count++;
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

int main(void)
{
   assert(AIMEE_PANEL_DRAFT == 0);
   assert(AIMEE_PANEL_REVIEW == 1);
   assert(AIMEE_PANEL_PARALLEL == 0);
   assert(AIMEE_PANEL_SEQUENTIAL == 1);

   agent_config_t agents = {0};
   config_t cfg = {0};
   aimee_panel_result_t result;
   memset(&result, 0x7f, sizeof result);

   assert(!aimee_panel_provider_available());
   assert(aimee_panel_run(&agents, &cfg, "task", NULL, &result) ==
          AIMEE_PANEL_PROVIDER_UNAVAILABLE);
   assert(result.artifact == NULL);
   assert(aimee_panel_provider_register(NULL) == AIMEE_PANEL_PROVIDER_INVALID);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(aimee_panel_provider_register(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(aimee_panel_provider_register(&other_provider) == AIMEE_PANEL_PROVIDER_BUSY);

   aimee_panel_aggregate_result_t aggregate;
   assert(aimee_panel_aggregate(&agents, &cfg, "answer", &aggregate) == AIMEE_PANEL_PROVIDER_OK);
   assert(aggregate.success == 1);
   assert(strcmp(aggregate.response, "answer") == 0);

   release_count = 0;
   run_mode = 0;
   assert(aimee_panel_run(&agents, &cfg, "artifact", NULL, &result) == AIMEE_PANEL_PROVIDER_OK);
   assert(strcmp(result.artifact, "artifact") == 0);
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_BUSY);
   aimee_panel_result_release(&result);
   aimee_panel_result_release(&result);
   assert(release_count == 1);

   run_mode = 1;
   assert(aimee_panel_run(&agents, &cfg, "failed", NULL, &result) == AIMEE_PANEL_PROVIDER_ERROR);
   assert(result.artifact == NULL);
   assert(release_count == 2);

   run_mode = 2;
   assert(aimee_panel_run(&agents, &cfg, "empty", NULL, &result) == AIMEE_PANEL_PROVIDER_ERROR);
   assert(result.artifact == NULL);
   assert(release_count == 2);

   assert(aimee_panel_provider_unregister(&other_provider) == AIMEE_PANEL_PROVIDER_INVALID);
   assert(aimee_panel_provider_unregister(&provider) == AIMEE_PANEL_PROVIDER_OK);
   assert(!aimee_panel_provider_available());
   assert(aimee_panel_aggregate(&agents, &cfg, "answer", &aggregate) ==
          AIMEE_PANEL_PROVIDER_UNAVAILABLE);
   assert(aggregate.response[0] == '\0');

   puts("panel_provider tests: ok");
   return 0;
}
