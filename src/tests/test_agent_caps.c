#include <assert.h>
#include <string.h>

#include "aimee.h"
#include "agent.h"
#include "model_registry.h"

void test_agent_route_with_caps_honors_tools_enabled(void)
{
   agent_config_t cfg;
   config_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.model_meta_capability_routing = 1;

   cfg.agent_count = 2;
   strcpy(cfg.default_agent, "minimax");

   strcpy(cfg.agents[0].name, "minimax");
   strcpy(cfg.agents[0].provider, "minimax");
   strcpy(cfg.agents[0].model, "MiniMax-M2.7");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   cfg.agents[0].tools_enabled = 1;
   strcpy(cfg.agents[0].api_key, "test-minimax-key");

   strcpy(cfg.agents[1].name, "no-tools");
   strcpy(cfg.agents[1].provider, "mistral");
   strcpy(cfg.agents[1].model, "mistral-large-latest");
   strcpy(cfg.agents[1].roles[0], "review");
   cfg.agents[1].role_count = 1;
   cfg.agents[1].enabled = 1;

   agent_t *routed = agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 0);
   assert(routed == &cfg.agents[0]);

   cfg.agents[0].tools_enabled = 0;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, MODEL_CAP_TOOLS, 0) == NULL);
}

/* Routing must honor the per-agent context_window override so onboarding a
 * model the capability catalog doesn't know about is a config change, not a
 * code change to the registry table. */
void test_agent_route_with_caps_honors_context_override(void)
{
   agent_config_t cfg;
   config_t sys_cfg;

   memset(&cfg, 0, sizeof(cfg));
   memset(&sys_cfg, 0, sizeof(sys_cfg));
   sys_cfg.model_meta_capability_routing = 1;

   cfg.agent_count = 1;
   strcpy(cfg.default_agent, "small-ctx");
   strcpy(cfg.agents[0].name, "small-ctx");
   strcpy(cfg.agents[0].provider, "openai");
   /* gpt-4's catalog context window is 8192 — below the 50000 requirement. */
   strcpy(cfg.agents[0].model, "gpt-4");
   strcpy(cfg.agents[0].roles[0], "review");
   cfg.agents[0].role_count = 1;
   cfg.agents[0].enabled = 1;
   strcpy(cfg.agents[0].api_key, "test-key");

   /* Catalog says 8192 < 50000 and no override -> dropped by the gate. */
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == NULL);

   /* An explicit per-agent override supersedes the catalog value and routes,
    * with no change to the model registry table. */
   cfg.agents[0].enabled = 1;
   cfg.agents[0].middleware.context_window = 1000000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == &cfg.agents[0]);

   /* An override below the requirement is still rejected. */
   cfg.agents[0].enabled = 1;
   cfg.agents[0].middleware.context_window = 1000;
   assert(agent_route_with_caps(&cfg, "review", &sys_cfg, 0, 50000) == NULL);
}
