/* wfe_def.h -- workflow definition model: a declarative graph of block
 * instances parsed from YAML. The block catalog (typed I/O per block) and the
 * validator live here too. See wfe_iface.h for the frozen execution seam.
 */
#ifndef DEC_WFE_DEF_H
#define DEC_WFE_DEF_H 1

#include <stddef.h>
#include "wfe_iface.h"
#include "cJSON.h"

#define WFE_ID_LEN   64
#define WFE_NAME_LEN 32
#define WFE_MAX_INS  8

/* One typed input binding: a logical input slot bound to a specific upstream
 * producer node's output handle (bind-by-instance). */
typedef struct
{
   char input_name[WFE_NAME_LEN];  /* logical slot, e.g. "src" or "pr" */
   char producer_id[WFE_ID_LEN];   /* node id producing the artifact */
   char output_name[WFE_NAME_LEN]; /* producer output handle (default "out") */
} wfe_binding_t;

typedef struct wfe_node
{
   char id[WFE_ID_LEN];
   wfe_block_type_t block;
   char custom_name[WFE_NAME_LEN]; /* set iff block == WFE_BLK_CUSTOM */
   const cJSON *params;            /* borrowed from def->raw; may be NULL */
   wfe_binding_t ins[WFE_MAX_INS];
   int n_ins;
   /* control edges (node ids; "" = none) */
   char next[WFE_ID_LEN];
   char on_pass[WFE_ID_LEN];
   char on_fail[WFE_ID_LEN];
} wfe_node_t;

typedef struct
{
   char name[WFE_ID_LEN];
   char start[WFE_ID_LEN]; /* entry node id (default: first node) */
   char version[65];       /* sha256 hex of canonical form (filled by compute_version) */
   wfe_node_t *nodes;
   int n_nodes;
   cJSON *raw; /* owned parsed tree */
} wfe_def_t;

/* ---- Block catalog ---- */
const char *wfe_block_name(wfe_block_type_t t);
wfe_block_type_t wfe_block_from_name(const char *name);
const char *wfe_artifact_name(wfe_artifact_type_t t);
/* output artifact a block produces (WFE_ART_NONE for merge/terminal). */
wfe_artifact_type_t wfe_block_output(wfe_block_type_t t);
/* 1 if `in` is an allowed input artifact type for block `t`. */
int wfe_block_accepts_input(wfe_block_type_t t, wfe_artifact_type_t in);
/* 1 if the block requires at least one bound input (author.proposal does not). */
int wfe_block_requires_input(wfe_block_type_t t);

/* Node-aware variants: resolve a custom block's typed I/O from the registry
 * (by node->custom_name); fall back to the built-in catalog otherwise. The
 * validator uses these so custom + built-in blocks type-check identically. */
wfe_artifact_type_t wfe_node_output(const wfe_node_t *n);
int wfe_node_accepts_input(const wfe_node_t *n, wfe_artifact_type_t in);
int wfe_node_requires_input(const wfe_node_t *n);

/* ---- Custom block registry (wfe_custom.c): built-ins ∪ $AIMEE_HOME/workflows/
 * blocks.yaml. Loaded lazily; the artifact type system + validator consult it. */
typedef enum
{
   WFE_EXEC_COMMAND = 0,
   WFE_EXEC_DELEGATE
} wfe_custom_exec_t;

#define WFE_CUSTOM_ARGV_MAX 32  /* max argv elements in a command executor */
#define WFE_CUSTOM_ARG_MAX  512 /* max bytes per argv element (reject, not truncate) */

typedef struct
{
   char name[WFE_NAME_LEN];
   wfe_artifact_type_t consumes; /* WFE_ART_NONE = source */
   wfe_artifact_type_t produces; /* branch or none only */
   wfe_custom_exec_t executor;
   /* OWNED (strdup'd) so the registry does not depend on the parsed YAML tree
    * staying alive -- no use-after-free if the tree is freed/reloaded. */
   char *argv[WFE_CUSTOM_ARGV_MAX + 1]; /* NULL-terminated (command executor) */
   int argc;
   char persona[WFE_NAME_LEN];
   char *prompt; /* owned or NULL (delegate executor) */
} wfe_custom_block_t;

/* Ensure the registry is loaded (idempotent; reads $AIMEE_HOME/workflows/
 * blocks.yaml if present + operator-owned). Returns 0 on success (incl. no
 * file), -1 if the file exists but is unsafe/invalid. */
int wfe_custom_registry_ensure(char *err, size_t errlen);
/* Force a (re)load from an explicit path (tests). */
int wfe_custom_registry_load(const char *path, char *err, size_t errlen);
void wfe_custom_registry_reset(void); /* test helper */
const wfe_custom_block_t *wfe_custom_lookup(const char *name);
int wfe_custom_count(void);
const wfe_custom_block_t *wfe_custom_at(int i);
/* Whether `command`-executor custom blocks are opt-in enabled (blocks.yaml
 * top-level allow_command: true). */
int wfe_custom_commands_allowed(void);

/* Wall-clock cap (ms) for a command block, from blocks.yaml command_timeout_ms
 * (default 120000). A command exceeding it is killed and the step fails. */
int wfe_custom_command_timeout_ms(void);

/* ---- Parse / free / lookup ---- */
wfe_def_t *wfe_def_parse(const char *yaml_text, char *err, size_t errlen);
wfe_def_t *wfe_def_load_file(const char *path, char *err, size_t errlen);
void wfe_def_free(wfe_def_t *def);
const wfe_node_t *wfe_def_node(const wfe_def_t *def, const char *id);

/* ---- Human-gate reject routing (see §5 of the lifecycle proposal) ----
 * Reject is terminal by DEFAULT; a gate opts into loop-back retry via
 * `retry_on_reject: true` in its params, following its `on_fail` edge. */
typedef enum
{
   WFE_GATE_REJECT_TERMINAL = 0, /* default: reject -> terminal `rejected` */
   WFE_GATE_REJECT_RETRY = 1,    /* opt-in: reject -> loop back to on_fail target */
   WFE_GATE_REJECT_ERR = -1      /* bad args (NULL def / NULL gate_id / target truncation) */
} wfe_gate_reject_t;

/* Decide how a reject at gate `gate_id` should route, purely from the parsed def
 * (no DB — unit-testable). Returns WFE_GATE_REJECT_RETRY iff the gate node exists,
 * its params set `retry_on_reject: true`, it has a non-empty `on_fail` edge, AND
 * that on_fail target resolves to a real node in `def` — then `out_target` is
 * filled with the target node id. In every other in-bounds case (no opt-in, no
 * on_fail edge, or a dangling/renamed on_fail target) it returns
 * WFE_GATE_REJECT_TERMINAL and does NOT write `out_target`. NULL `def`/`gate_id`,
 * or an `out_target` too small to hold the target id, return WFE_GATE_REJECT_ERR. */
wfe_gate_reject_t wfe_gate_reject_target(const wfe_def_t *def, const char *gate_id,
                                         char *out_target, size_t out_n);

/* ---- Validate (wfe_validate.c). Returns 0 on success; nonzero + err set. ---- */
int wfe_def_validate(const wfe_def_t *def, char *err, size_t errlen);

/* ---- Canonical form + version (wfe_canonical.c) ---- */
/* Deterministic canonical YAML (stable under cycles). malloc'd; caller frees. */
char *wfe_def_canonical(const wfe_def_t *def);
/* sha256 hex of the canonical form into out_hex (65 bytes incl NUL). */
int wfe_def_compute_version(const wfe_def_t *def, char out_hex[65]);
/* sha256 hex helper. Returns 0 on success. */
int wfe_sha256_hex(const void *data, size_t len, char out_hex[65]);
/* sha256 raw 32-byte digest (used for HMAC in the approval signer). */
int wfe_sha256_raw(const void *data, size_t len, unsigned char out[32]);

#endif /* DEC_WFE_DEF_H */
