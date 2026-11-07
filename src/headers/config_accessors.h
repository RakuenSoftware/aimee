/* Caller contract expanded from config_accessor_keys.def.
 *
 * These narrow accessors read the independently running pure-Go config module
 * over Aimee's existing event bus. No native configuration object or parser is
 * exposed: callers ask for the scalar or array element they need.
 */

#ifndef DEC_CONFIG_ACCESSORS_H
#define DEC_CONFIG_ACCESSORS_H 1

#include <stddef.h> /* size_t, for the _copy forms */
#include <stdint.h>
#include <time.h>

/* A buffer of this size holds ANY string field whole, so a caller using it
 * with a _copy accessor never truncates and never needs an implementation type.
 * spell the field's width. Generated as the widest string field. */
#define CONFIG_COPY_MAX 4096

/* Go resolves stored defaults. The table also records failure defaults for native
 * callers; security gates remain enabled when the transport cannot answer. */
#define CONFIG_NUMBER(name, type, key, fallback) type config_##name(void);
#define CONFIG_STRING(name, key, width)                                                            \
   const char *config_##name(void);                                                                \
   size_t config_##name##_copy(char *out, size_t n);
#define CONFIG_INDEX_NUMBER(name, type, key, field)  type config_##name(int index);
#define CONFIG_INDEX_STRING(name, key, field, width) const char *config_##name(int index);
#include "config_accessor_keys.def"
#undef CONFIG_NUMBER
#undef CONFIG_STRING
#undef CONFIG_INDEX_NUMBER
#undef CONFIG_INDEX_STRING

int config_mcp_client_count(void);
const char *config_server_api_bearer_extra(int index);
int config_server_api_bearer_extra_count(void);

#endif
