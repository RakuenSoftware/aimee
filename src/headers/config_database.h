#ifndef DEC_CONFIG_DATABASE_H
#define DEC_CONFIG_DATABASE_H

#include <stddef.h>

int config_db2_url_effective(char *out, size_t n);
int config_embedder_dims_default(void);
int config_resolve_embedder_dims_current(void);
int config_embedder_dims_current(void);
int config_synth_chat_endpoint_current(char *out, size_t out_len);
int config_embedder_dims_pinned_current(void);
void config_emit_deploy_env_current(char *buf, size_t n);

#endif
