/* Focused configuration accessor fixture for curator provider tests. */
#ifndef TESTS_SUPPORT_CURATOR_CONFIG_STUB_H
#define TESTS_SUPPORT_CURATOR_CONFIG_STUB_H
#include <stddef.h>

typedef struct
{
   char kb_curator_provider_base_url[256];
   char kb_curator_provider_model[128];
   char kb_curator_provider_api_key[256];
   char synthesis_endpoint[512];
   int synthesis_thinking;
   int kb_curator_extract_max_tokens;
} curator_config_stub_t;

/* Named `cfg` so existing cases read unchanged. */
extern curator_config_stub_t cfg;

#endif /* TESTS_SUPPORT_CURATOR_CONFIG_STUB_H */
