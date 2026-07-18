/* roundtable_preset.h: server-owned registry of NAMED roundtable presets.
 *
 * A preset captures the full roundtable panel spec — the seats (paired
 * model+persona), the aggregator, the guard/loop knobs, and the authoring
 * pipeline knobs — so the web GUI can maintain several named configurations and
 * pick which one is "active". Selecting a preset active copies its values into
 * the live config_t ensemble_* and roundtable_* fields (the runtime source of truth,
 * read by delegate_ensemble.c) and records the name in config_t.roundtable_default;
 * the roundtable RUNTIME is otherwise untouched.
 *
 * Presets are stored one-per-file as JSON at <config_default_dir()>/roundtables/
 * <name>.json, mirroring the persona registry (persona.{c,h}). Server-side only —
 * the thin client reaches presets over the /v1 HTTP API, never by reading files. */
#ifndef DEC_ROUNDTABLE_PRESET_H
#define DEC_ROUNDTABLE_PRESET_H 1

#include "cJSON.h"
#include <stddef.h>

/* Bounds mirror the config_t arrays (config.h). ENSEMBLE_MAX_REFS == 32 seats;
 * model/persona/aggregator widths match the config_t field sizes exactly so an
 * apply-to-config copy never truncates differently than the config parser. */
#define RT_PRESET_MAX_SEATS   32
#define RT_PRESET_NAME_MAX    64
#define RT_PRESET_MODEL_MAX   128
#define RT_PRESET_PERSONA_MAX 64
#define RT_PRESET_DESC_MAX    256
#define RT_PRESET_PATH_MAX    1024

typedef struct
{
   char model[RT_PRESET_MODEL_MAX];     /* configured agent/delegate name */
   char persona[RT_PRESET_PERSONA_MAX]; /* review persona for this seat ("" = engine default) */
} rt_preset_seat_t;

/* Plain struct (no heap pointers): memset-zeroing and struct copy stay valid. */
typedef struct
{
   char name[RT_PRESET_NAME_MAX];
   char description[RT_PRESET_DESC_MAX];

   rt_preset_seat_t seats[RT_PRESET_MAX_SEATS];
   int seat_count;

   char aggregator[RT_PRESET_MODEL_MAX];
   int min_successful;
   double max_cost_usd;

   int max_rounds;
   int converge_threshold;
   int deadline_ms;
   char turns[16]; /* "parallel" | "sequential" */

   /* Authoring pipeline (roundtable.pipeline_*). */
   char pipeline_done_bar[40];
   int pipeline_max_passes;
   int pipeline_max_attempts_per_pass;
   double pipeline_max_cost_usd;
   double pipeline_max_total_cost_usd;
   int pipeline_gate_ttl_h;
   int pipeline_parked_releases_slot;
   int pipeline_unknown_context_tokens;
} roundtable_preset_t;

/* Name charset: same file-safe rules as persona_name_valid (alnum . _ - ; no
 * leading dot, no "." / ".."). Returns 1 if valid, 0 otherwise. */
int roundtable_preset_name_valid(const char *name);

/* List preset names found under <config>/roundtables/. Returns the count written
 * to names_out (deduplicated), or 0 on error / empty. */
int roundtable_preset_list(char names_out[][RT_PRESET_NAME_MAX], int max_names);

/* Load a preset by name into *out (zeroed first). Returns 0 on success, -1 if the
 * file is missing or unparseable. */
int roundtable_preset_load(const char *name, roundtable_preset_t *out);

/* Persist *p to <config>/roundtables/<p->name>.json (creating the directory).
 * Returns 0 on success, -1 on validation / IO error. */
int roundtable_preset_save(const roundtable_preset_t *p);

/* Delete <config>/roundtables/<name>.json. Returns 0 on success, -1 if absent. */
int roundtable_preset_delete(const char *name);

/* Serialize *p to a JSON object the caller owns (cJSON_Delete). */
cJSON *roundtable_preset_to_json(const roundtable_preset_t *p);

/* Parse a create/edit request body into *out (zeroed first). `url_name`, when
 * non-empty, takes precedence over a body "name". Returns 0 on success; -1 with
 * *errmsg set (static string) on a validation error. */
int roundtable_preset_from_json(const char *body, const char *url_name, roundtable_preset_t *out,
                                const char **errmsg);

/* Make `name` the active preset: load it, copy its fields into the on-disk
 * config's ensemble_* and roundtable_* fields, set roundtable_default = name, and
 * config_save + config_reload so the change takes effect live. Returns 0 on
 * success; -1 with a message in err (if err != NULL) on failure. */
int roundtable_preset_apply_to_config(const char *name, char *err, size_t errn);

/* Synthesize a preset named `name` from the live config_t (ensemble_* and roundtable_*)
 * into *out. Used to materialize an implicit "current" preset when the store is
 * empty, so the GUI opens showing today's effective roundtable. Never touches disk. */
void roundtable_preset_from_current_config(const char *name, roundtable_preset_t *out);

#endif /* DEC_ROUNDTABLE_PRESET_H */
