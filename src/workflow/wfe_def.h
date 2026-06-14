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
   const cJSON *params; /* borrowed from def->raw; may be NULL */
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

/* ---- Parse / free / lookup ---- */
wfe_def_t *wfe_def_parse(const char *yaml_text, char *err, size_t errlen);
wfe_def_t *wfe_def_load_file(const char *path, char *err, size_t errlen);
void wfe_def_free(wfe_def_t *def);
const wfe_node_t *wfe_def_node(const wfe_def_t *def, const char *id);

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
