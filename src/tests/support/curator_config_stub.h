/* curator_config_stub.h -- the config fields kb_curator_provider_for_stage and
 * kb_curator_llm_run read, for suites that link neither config.c nor the generated
 * accessors.
 *
 * Those two resolve config through accessors now rather than taking a config_t, so
 * a test can no longer just hand one over. Linking the real accessors is not an
 * option here either: they call config_field_read, which drags config.c and the
 * whole YAML loader into suites that exist precisely to avoid it.
 *
 * So the accessors are stubbed against this struct. Field names match config_t's,
 * so a case still sets exactly the field it means to, by the name it always used.
 * Zero it (memset) at the top of each case; nothing else reads it. */
#ifndef TESTS_SUPPORT_CURATOR_CONFIG_STUB_H
#define TESTS_SUPPORT_CURATOR_CONFIG_STUB_H

typedef struct
{
   char kb_curator_provider_base_url[256];
   char kb_curator_provider_model[128];
   char kb_curator_provider_api_key[256];
   char kb_curator_tier_b_base_url[256];
   char kb_curator_tier_b_model[128];
   char kb_curator_tier_b_api_key[256];
   char llm_synth_endpoint[512];
   int kb_curator_extract_max_tokens;
} curator_config_stub_t;

/* Named `cfg` so existing cases read unchanged. */
extern curator_config_stub_t cfg;

#endif /* TESTS_SUPPORT_CURATOR_CONFIG_STUB_H */
