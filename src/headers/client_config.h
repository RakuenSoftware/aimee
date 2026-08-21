#ifndef AIMEE_CLIENT_CONFIG_H
#define AIMEE_CLIENT_CONFIG_H

#include "cJSON.h"

/* Read one public setting from the configured Aimee server. The server owns the
 * config-module event-bus call; thin clients never inspect its YAML storage. */
cJSON *client_config_value(const char *key);

/* Narrow test/application seam. Passing NULL restores the remote-server path. */
void client_config_set_provider(cJSON *(*provider)(const char *key));
void client_config_set_operation_provider(cJSON *(*provider)(const char *operation,
                                                             const cJSON *value));

/* Mutations travel through the server's authenticated config.set route; the
 * server then invokes the external module over its event bus. Takes ownership
 * of value. */
int client_config_operation(const char *operation, cJSON *value);
int client_config_profile_present(const char *name);
/* Return an owned array of profile-name strings, or NULL on failure. */
cJSON *client_config_profile_list(void);

int client_config_bool(const char *key, int fallback);
int client_config_int(const char *key, int fallback);
int client_config_string(const char *key, char *out, unsigned long out_size, const char *fallback);

#endif
