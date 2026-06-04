#ifndef CONFIG_FIELDS_H
#define CONFIG_FIELDS_H

#include "cJSON.h"
#include "config.h"
#include <stddef.h>

/* config_fields: the shared allowlist of get/set-able top-level config_t
 * fields, keyed by name. Extracted from cmd_data.c so that BOTH the (legacy)
 * `aimee config` command and the server's config.show/get/set handlers operate
 * on a single table. CORE layer: depends only on config.h and cJSON. */

typedef enum
{
   CFG_STRING,
   CFG_BOOL,
   CFG_INT,
   CFG_FLOAT
} config_field_type_t;

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool; /* 1 for bool fields */
   config_field_type_t type;
} config_field_t;

/* NULL-key-terminated allowlist. */
extern const config_field_t config_fields[];

/* Look up a field by key, or NULL if it is not in the allowlist. */
const config_field_t *config_field_lookup(const char *key);

/* Build a cJSON node holding the field's current value (bool -> Bool,
 * int/float -> Number, string -> String). Never reads past the field. */
cJSON *config_field_value_json(const config_t *cfg, const config_field_t *f);

/* Parse `value` and assign it into the field. Returns 0 on success, -1 on an
 * invalid value (e.g. non-boolean text for a bool field). */
int config_field_set_value(config_t *cfg, const config_field_t *f, const char *value);

#endif /* CONFIG_FIELDS_H */
