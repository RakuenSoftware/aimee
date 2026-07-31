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
   CFG_FLOAT,
   CFG_ECON_MODE /* int enum stored, but get/set as "off|safe|aggressive" */
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

/* Surface classification, so the everyday config surface (CLI `config show`,
 * the Settings GUI, the generated reference "CLI-settable keys" count) presents
 * only the keys a normal user tunes. Non-RUNTIME keys stay fully get/set-able —
 * they are just filed out of the presented surface. Omitted -> FGROUP_RUNTIME (0). */
typedef enum
{
   FGROUP_RUNTIME = 0, /* everyday user-facing knob (default) */
   FGROUP_DEPLOY,      /* deploy-time infra topology: consumed once by
                          config_emit_deploy_env to stand up the managed sibling
                          services, never read at runtime. Set at deploy, not
                          tuned day-to-day. */
   FGROUP_ADVANCED,    /* expert tuning scalar with a good default; file-settable, off surface */
   FGROUP_DEV,         /* dev/QA-only (e.g. dogfood_*); not part of the user surface */
} config_field_group_t;

typedef struct
{
   const char *key;
   size_t offset;
   size_t size;
   int is_bool; /* 1 for bool fields */
   config_field_type_t type;
   reload_class_t reload_class; /* omitted -> RELOAD_HOT (0) */
   config_field_group_t group;  /* omitted -> FGROUP_RUNTIME (0) */
   /* Canonical Vault/runtime-cache record for a credential compatibility
    * field. NULL means the field is ordinary configuration. Credential values
    * may exist in config_t transiently for consumers, but must never be
    * serialized or returned by a generic config surface. */
   const char *secret_name;
} config_field_t;

/* Human label for the reload class, for the config.set / Settings verdict. */
const char *config_field_reload_verdict(const config_field_t *f);

/* Surface-group name ("runtime" | "deploy" | "advanced" | "dev") for a field,
 * so config.show can advertise which keys sit off the everyday surface and the
 * Settings GUI can hide them by default. */
const char *config_field_group_name(const config_field_t *f);

/* NULL-key-terminated allowlist. */
extern const config_field_t config_fields[];

/* Look up a field by key, or NULL if it is not in the allowlist. */
const config_field_t *config_field_lookup(const char *key);

/* Build a cJSON node holding the field's current value (bool -> Bool,
 * int/float -> Number, string -> String). Never reads past the field. */
cJSON *config_field_value_json(const config_t *cfg, const config_field_t *f);

/* Return the canonical Vault record name for a credential field, or NULL for
 * ordinary configuration. Public config surfaces must use
 * config_field_public_value_json(), which reports only configured/unconfigured
 * for credentials and never returns their value. */
const char *config_field_secret_name(const config_field_t *f);
cJSON *config_field_public_value_json(const config_t *cfg, const config_field_t *f);

/* Live-config form; prefer this outside the config module. */
cJSON *config_field_public_value_json_current(const config_field_t *f);

/* Render one field for display without a config_t. Same text `aimee config get`
 * always printed: "(unset)" for an empty string, "configured"/"not configured"
 * for a Vault-backed secret (never the value). Returns 0 on success. */
int config_field_render(const config_field_t *f, char *out, size_t n);

/* Parse `value` and assign it into the field. Returns 0 on success, -1 on an
 * invalid value (e.g. non-boolean text for a bool field). */
int config_field_set_value(config_t *cfg, const config_field_t *f, const char *value);

/* Apply the table-driven defaults for every FLAT config field into cfg. Called by
 * config_set_defaults so a flat field's default lives only in config_flat_defaults[].
 * Non-flat defaults remain in config_set_defaults. */
void config_apply_flat_defaults(config_t *cfg);

/* Table-driven parse of the flat scalar config fields from a parsed YAML root
 * (Proposal A, step 3). Assigns each present, correctly-typed flat field; matches
 * the inline config_load parse. config_load calls this instead of the per-field
 * blocks. (css_render_command is excluded and keeps its bespoke block.) */
void config_parse_flat_fields(config_t *cfg, const cJSON *root);

#endif /* CONFIG_FIELDS_H */
