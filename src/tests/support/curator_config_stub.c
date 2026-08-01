/* Accessor stubs over curator_config_stub_t -- see the header for why these exist
 * rather than linking the generated accessors. */
#include "curator_config_stub.h"

curator_config_stub_t cfg;

const char *config_kb_curator_provider_base_url(void)
{
   return cfg.kb_curator_provider_base_url;
}

const char *config_kb_curator_provider_model(void)
{
   return cfg.kb_curator_provider_model;
}

const char *config_kb_curator_provider_api_key(void)
{
   return cfg.kb_curator_provider_api_key;
}

const char *config_kb_curator_tier_b_base_url(void)
{
   return cfg.kb_curator_tier_b_base_url;
}

const char *config_kb_curator_tier_b_model(void)
{
   return cfg.kb_curator_tier_b_model;
}

const char *config_kb_curator_tier_b_api_key(void)
{
   return cfg.kb_curator_tier_b_api_key;
}

const char *config_llm_synth_endpoint(void)
{
   return cfg.llm_synth_endpoint;
}

int config_kb_curator_extract_max_tokens(void)
{
   return cfg.kb_curator_extract_max_tokens;
}
