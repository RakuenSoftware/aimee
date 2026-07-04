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

/* When a config.set / Settings change takes effect (live-config-reload P2). Default 0 = HOT.
 * HOT-default is justified, not fail-open: with P1b, config_load returns the pushed snapshot,
 * so every field READ PER-REQUEST is live immediately — and the audited majority (provider/
 * model/endpoint, and the reduce/economizer/memory/ingress feature flags) are read per
 * request (e.g. openai_endpoint/embedding_endpoint are read on each call). The STARTUP-BOUND
 * minority is explicitly RELOAD_RESTART: db2_url (postgres pool), kb_api_* (kb client init),
 * autonomy.* (env bridge). As P3 adds re-appliers, those move RESTART -> REAPPLIABLE (live). */
typedef enum
{
   RELOAD_HOT = 0,     /* read per-request -> live immediately after config.set pushes a reload */
   RELOAD_REAPPLIABLE, /* bound state with a live re-applier hook (P3) */
   RELOAD_RESTART,     /* bound at startup with no live re-applier yet -> needs a restart */
} reload_class_t;

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool; /* 1 for bool fields */
   config_field_type_t type;
   reload_class_t reload_class; /* omitted -> RELOAD_HOT (0) */
} config_field_t;

/* Human label for the reload class, for the config.set / Settings verdict. */
const char *config_field_reload_verdict(const config_field_t *f);

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
