#ifndef AIMEE_CONFIG_CLIENT_H
#define AIMEE_CONFIG_CLIENT_H

#include <stddef.h>
#include "cJSON.h"

#define AIMEE_CONFIG_EVENT_KIND 4609u
#define AIMEE_CONFIG_STAGE_ID   1u

int config_client_refresh(void);
/* Return 1 when the module's document version differs from the cached
 * snapshot, 0 when unchanged, and -1 on transport/protocol failure. */
int config_client_changed(void);
int config_client_read_number(const char *key, double *out);
int config_client_read_string(const char *key, char *out, size_t n);
int config_client_read_indexed_number(const char *key, int index, const char *member, double *out);
int config_client_read_indexed_string(const char *key, int index, const char *member, char *out,
                                      size_t n);
cJSON *config_client_value_copy(const char *key);
cJSON *config_client_snapshot_copy(void);
int config_client_set_number(const char *key, double value);
int config_client_set_string(const char *key, const char *value);
int config_client_set_value(const char *key, cJSON *value);
/* Invoke a named module-owned mutation. Takes ownership of value when non-NULL.
 * Returns -2 for exists/not-found and -3 for a full bounded registry. */
int config_client_operation(const char *operation, cJSON *value);
const char *config_client_last_error(void);
int config_client_key_is_secret(const char *key);
const char *config_client_secret_name(const char *key);

#endif
