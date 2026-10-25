/* aimee_ir.h -- the canonical, protocol-NEUTRAL intermediate representation for
 * LLM requests/responses. FRONTEND adapters (client wire ↔ IR) and BACKEND adapters
 * (IR ↔ provider wire) both pivot through this; there is NO direct client-shape →
 * provider-shape translation. Design: docs/proposals/pending/aimee-canonical-ir.md
 * (roundtabled 2026-07-01).
 *
 * HYBRID by ruling: a typed core (semantics explicit + testable) PLUS a retained
 * `raw` cJSON sidecar on each object, so unknown provider fields survive a
 * same-protocol round-trip (a pure struct would silently drop them and break
 * prompt-cache / correctness). The typed core is the source of truth for the core
 * stages; the sidecar is for lossless same-protocol replay + observability.
 *
 * TRUST-BOUNDARY RULE: tool names and ids are opaque typed fields, never derived
 * from arguments. Completed tool input is admitted once as canonical JSON. Its
 * exact decimal tokens survive cloning, equality and every provider rebuild;
 * known numeric fields independently validate their numeric bounds. Immutable
 * native sidecars serve only verified same-protocol output replay and never
 * override the owned canonical request. Complete native argument strings require
 * full JSON framing. Streaming accumulates raw fragments until block completion;
 * an in-flight tool use is not IR-equal to its finalized form. */

#ifndef DEC_AIMEE_IR_H
#define DEC_AIMEE_IR_H 1

#include <stddef.h>

struct cJSON;

#define AIMEE_IR_MAX_BLOCKS 4096

/* Which wire protocol an adapter speaks (frontend: the client; backend: provider). */
typedef enum
{
   AIMEE_WIRE_UNKNOWN = 0,
   AIMEE_WIRE_ANTHROPIC,   /* Anthropic Messages API (/v1/messages) */
   AIMEE_WIRE_OPENAI_CHAT, /* OpenAI Chat Completions (/v1/chat/completions) */
   AIMEE_WIRE_RESPONSES    /* OpenAI Responses API (/v1/responses; codex) */
   /* No GEMINI wire: Gemini is reached through its OpenAI-compatible endpoint, so
    * it is AIMEE_WIRE_OPENAI_CHAT like any other OpenAI-shaped provider. The IR
    * never needed a bespoke Gemini protocol. */
} aimee_wire_t;

/* A content block. Ordered within a message/response; ordering is significant. */
typedef enum
{
   AIMEE_BLK_TEXT = 0,
   AIMEE_BLK_TOOL_USE,    /* assistant asks to call a tool */
   AIMEE_BLK_TOOL_RESULT, /* user/tool returns a tool's output */
   AIMEE_BLK_IMAGE,
   AIMEE_BLK_DOCUMENT,
   AIMEE_BLK_THINKING, /* Anthropic extended-thinking / o-series reasoning */
   AIMEE_BLK_UNKNOWN   /* preserved via `raw` only */
} aimee_block_type_t;

/* Context authority is orthogonal to content type. A TEXT block may be a
 * user task instruction, retrieved evidence, or an operator-only diagnostic;
 * flattening those into one untyped prompt is an authority escalation. */
typedef enum
{
   AIMEE_CTX_ORIGIN_UNKNOWN = 0,
   AIMEE_CTX_ORIGIN_PLATFORM,
   AIMEE_CTX_ORIGIN_OPERATOR,
   AIMEE_CTX_ORIGIN_USER,
   AIMEE_CTX_ORIGIN_TOOL,
   AIMEE_CTX_ORIGIN_RETRIEVAL,
   AIMEE_CTX_ORIGIN_MEMORY,
   AIMEE_CTX_ORIGIN_MODEL
} aimee_context_origin_t;

typedef enum
{
   AIMEE_CTX_AUTH_DIAGNOSTIC = 0,
   AIMEE_CTX_AUTH_EVIDENCE,
   AIMEE_CTX_AUTH_TASK_INSTRUCTION,
   AIMEE_CTX_AUTH_POLICY
} aimee_context_authority_t;

typedef enum
{
   AIMEE_CTX_TRUST_UNKNOWN = 0,
   AIMEE_CTX_TRUST_UNTRUSTED,
   AIMEE_CTX_TRUST_UNVERIFIED,
   AIMEE_CTX_TRUST_REVIEWED,
   AIMEE_CTX_TRUST_VERIFIED
} aimee_context_trust_t;

typedef enum
{
   AIMEE_CTX_SENS_PUBLIC = 0,
   AIMEE_CTX_SENS_INTERNAL,
   AIMEE_CTX_SENS_CONFIDENTIAL,
   AIMEE_CTX_SENS_RESTRICTED
} aimee_context_sensitivity_t;

typedef struct
{
   aimee_context_origin_t origin;
   aimee_context_authority_t authority;
   aimee_context_trust_t trust;
   aimee_context_sensitivity_t sensitivity;
   int model_visible;
   char revision_domain[48];
   char revision_scope[128];
   unsigned long long revision_epoch;
} aimee_context_meta_t;

typedef struct
{
   aimee_block_type_t type;
   aimee_context_meta_t context;
   /* TEXT / THINKING */
   char *text;
   /* THINKING: the provider's opaque signature over the reasoning block. Anthropic
    * REQUIRES it echoed back verbatim on a resubmitted assistant thinking turn, so it
    * must be modeled (not lost to the raw sidecar) for the canonical egress. Owned. */
   char *thinking_signature;
   /* Host replay fence over the admitted native prefix and thinking chain.
    * This is provenance metadata, never a replacement vendor signature. */
   char *thinking_binding;
   char *thinking_id;
   char *thinking_encrypted_content;
   char *thinking_redacted;
   /* TOOL_USE: id = the call id (stable, links to the matching TOOL_RESULT);
    * name = opaque tool name; input = opaque argument JSON (borrowed sidecar view,
    * preserved verbatim). TOOL_RESULT: id = the tool_use id it answers;
    * result = opaque content; is_error set on an error result. */
   char *tool_id;
   char *tool_name;
   /* TOOL_USE: the tool's owning namespace group, NULL when it has none. A Codex
    * client offers its MCP tools inside a `namespace` group and routes on
    * (namespace, name) together, so the pair must stay together through a
    * resubmitted history. Modeled rather than left to `raw` for the same reason as
    * thinking_signature: `raw` is same-protocol replay, and this has to survive the
    * chat hop in the middle of responses -> IR -> chat -> IR -> responses. Owned. */
   char *tool_namespace;
   struct cJSON *tool_input;  /* owned */
   struct cJSON *tool_result; /* owned */
   int tool_is_error;
   /* IMAGE / DOCUMENT: media_type (e.g. "image/png"), and either a base64 payload
    * or a URL in media_ref. */
   char *media_type;
   char *media_ref;
   /* Per-block prompt-cache marker (opaque, e.g. "ephemeral"); NULL if none. The
    * cache decision is per-section, so this must survive block-preserving stages. */
   char *cache_control;
   char *cache_ttl; /* optional cache lifetime, e.g. 5m or 1h; owned */
   /* Positive groups retain content belonging to one authored message. Zero
    * leaves grouping unspecified; paragraph presentation never changes text bytes. */
   int message_group;
   /* Sidecar: the block's original wire JSON (unknown fields, exact bytes for
    * same-protocol replay). Owned. */
   struct cJSON *raw;
} aimee_block_t;

typedef struct
{
   char *role; /* opaque: "user" / "assistant" / "system" / "tool" / ... */
   aimee_block_t *blocks;
   int n_blocks;
   int content_scalar;      /* observed scalar-text form; retained as transport provenance */
   struct cJSON *raw;       /* sidecar */
   int compaction_boundary; /* canonical history marker; never provider content */
} aimee_message_t;

typedef enum
{
   AIMEE_STRICT_UNSET = 0,
   AIMEE_STRICT_NULL,
   AIMEE_STRICT_FALSE,
   AIMEE_STRICT_TRUE
} aimee_strict_mode_t;

typedef enum
{
   AIMEE_TOOL_FUNCTION = 0,
   AIMEE_TOOL_NAMESPACE,
   AIMEE_TOOL_HOSTED
} aimee_tool_kind_t;

typedef struct aimee_tool
{
   char *name;
   char *description;
   struct cJSON *schema; /* owned opaque function argument schema */
   char *cache_control;
   char *cache_ttl;
   aimee_tool_kind_t kind;
   struct aimee_tool *children; /* namespace functions; owned */
   int n_children;
   /* Provider-owned execution/configuration is explicit, never a nameless function. */
   aimee_wire_t provider_wire;
   char *native_type;
   struct cJSON *native_options;
   aimee_strict_mode_t strict;
   int has_defer_loading, defer_loading;
   struct cJSON *raw; /* entry sidecar, never the source of the typed catalog */
} aimee_tool_t;

void aimee_tool_free(aimee_tool_t *tool);
int aimee_ir_tool_valid(const aimee_tool_t *tool);

/* Which tools the model may or must call. TYPED, not wire-shaped: every wire spells
 * this differently (Anthropic {type:auto|any|tool|none}; OpenAI chat
 * "auto"|"none"|"required"|{type:function,function:{name}}; Responses
 * "auto"|"none"|"required"|{type:function,name}), so an opaque copy is only valid on
 * the wire it was parsed from. A choice no wire-neutral value can express (e.g. a
 * Responses hosted-tool or allowed_tools choice) is a frontend parse error, not a
 * silent downgrade. */
typedef enum
{
   AIMEE_TOOL_CHOICE_UNSET = 0, /* the client sent none: provider default */
   AIMEE_TOOL_CHOICE_AUTO,
   AIMEE_TOOL_CHOICE_NONE,
   AIMEE_TOOL_CHOICE_ANY,  /* must call some tool (Anthropic any / OpenAI required) */
   AIMEE_TOOL_CHOICE_TOOL, /* must call tool_choice_name */
} aimee_tool_choice_t;

typedef enum
{
   AIMEE_OUTPUT_UNSET = 0,
   AIMEE_OUTPUT_TEXT,
   AIMEE_OUTPUT_JSON_OBJECT,
   AIMEE_OUTPUT_JSON_SCHEMA
} aimee_output_kind_t;

typedef struct
{
   aimee_output_kind_t kind;
   char *name, *description;
   struct cJSON *schema; /* owned, opaque JSON Schema */
   aimee_strict_mode_t strict;
} aimee_output_format_t;

typedef enum
{
   AIMEE_CACHE_DEFAULT =
       0, /* retain native client policy; translate other sources deterministically */
   AIMEE_CACHE_STABLE_PREFIX, /* explicit host normalization policy */
} aimee_cache_policy_t;

typedef enum
{
   AIMEE_EFFORT_UNSET = 0,
   AIMEE_EFFORT_NONE,
   AIMEE_EFFORT_MINIMAL,
   AIMEE_EFFORT_LOW,
   AIMEE_EFFORT_MEDIUM,
   AIMEE_EFFORT_HIGH,
   AIMEE_EFFORT_XHIGH,
   AIMEE_EFFORT_MAX
} aimee_reasoning_effort_t;
const char *aimee_ir_effort_name(aimee_reasoning_effort_t effort);
int aimee_ir_effort_parse(const char *name, aimee_reasoning_effort_t *effort);

typedef struct
{
   char *model;
   aimee_cache_policy_t cache_policy;
   aimee_block_t *system; /* system as ORDERED BLOCKS, not a flat string */
   int n_system;
   aimee_message_t *messages;
   int n_messages;
   aimee_tool_t *tools;
   int n_tools;
   aimee_output_format_t output_format;
   aimee_reasoning_effort_t reasoning_effort;
   char *reasoning_summary; /* owned: auto, concise or detailed */
   aimee_tool_choice_t tool_choice;
   char *tool_choice_name; /* AIMEE_TOOL_CHOICE_TOOL only; opaque tool name; owned */
   /* Whether the model may emit several tool calls in one turn. OpenAI and Responses
    * spell it parallel_tool_calls; Anthropic spells the negation,
    * tool_choice.disable_parallel_tool_use. has_* == 0 when the client sent neither. */
   int parallel_tool_calls;
   int has_parallel_tool_calls;
   int max_tokens;
   int has_max_tokens;
   double temperature;
   int has_temperature;
   /* Sampling params clients send at the top level. Modeled (not raw-carried) so the
    * canonical egress can re-emit them deterministically once the raw sidecar is
    * retired -- and so they survive cross-protocol translation. top_p/top_k are valid
    * on both Anthropic and OpenAI; a source that omits one leaves has_* == 0. */
   double top_p;
   int has_top_p;
   int top_k;
   int has_top_k;
   int stream;
   char **stop_sequences;
   int n_stop;
   /* Opaque top-level request metadata (e.g. Anthropic `metadata.user_id`), preserved
    * verbatim through the IR so the canonical egress is byte-faithful without the raw
    * sidecar. Owned. NULL when the client sent none. */
   struct cJSON *metadata;
   /* Anthropic `service_tier` ("auto"/"standard_only"/...); NULL if unset. Owned. */
   char *service_tier;
   /* OpenAI-family cache routing and retention controls. Owned; NULL means
    * provider default. Options remain opaque JSON with exact numeric tokens;
    * retention and options express independent policies, never rewrite one
    * into the other. Native raw sidecars are not their authority. */
   char *prompt_cache_key;
   char *prompt_cache_retention;
   struct cJSON *prompt_cache_options;
   /* Anthropic extended-thinking CONFIG object ({type:"enabled",budget_tokens:N}) -- a
    * top-level request field, distinct from THINKING content blocks in messages.
    * Opaque, preserved verbatim. Owned. NULL when the client sent none. */
   struct cJSON *thinking;
   aimee_wire_t frontend; /* the client wire this was parsed from */
   /* A transform reports typed payload changes here. Native replay additionally
    * compares the complete modeled wire content against its entry snapshot;
    * provenance-only edits do not change native bytes, and a missed flag cannot
    * replay stale payloads. */
   int mutated;
   /* Whole-request sidecar: the ORIGINAL request JSON. Enables the same-protocol
    * raw-passthrough fast-path (frontend==backend, unmutated) and lossless replay.
    * Owned. */
   struct cJSON *raw;
   char *raw_bytes; /* original native request bytes; owned, excluded from model/log JSON */
   size_t raw_length;
} aimee_request_t;

/* Canonical stop reasons; the provider-specific string is kept alongside so nothing
 * is lost to logs/observability. */
typedef enum
{
   AIMEE_STOP_UNKNOWN = 0,
   AIMEE_STOP_END_TURN, /* natural stop */
   AIMEE_STOP_MAX_TOKENS,
   AIMEE_STOP_TOOL_USE, /* stopped to call a tool */
   AIMEE_STOP_STOP_SEQUENCE,
   AIMEE_STOP_CONTENT_FILTER,
   AIMEE_STOP_ERROR
} aimee_stop_reason_t;

/* Optional terminal diagnostics, never message content or execution authority.
 * NULL fields are meaningful; unknown categories remain bounded inert data. */
#define AIMEE_REFUSAL_CATEGORY_MAX    64u
#define AIMEE_REFUSAL_EXPLANATION_MAX 4096u
typedef struct
{
   char *category;
   char *explanation;
} aimee_refusal_details_t;
int aimee_refusal_details_valid(const aimee_refusal_details_t *details);
aimee_refusal_details_t *aimee_refusal_details_clone(const aimee_refusal_details_t *details);
void aimee_refusal_details_free(aimee_refusal_details_t *details);
/* Neutral JSON uses exactly nullable category/explanation. NULL represents no
 * details. Decode replaces initialized owned storage only after validation. */
struct cJSON *aimee_refusal_details_json(const aimee_refusal_details_t *details);
int aimee_refusal_details_parse(const struct cJSON *json, aimee_refusal_details_t **out);

typedef struct aimee_response
{
   char *id;
   char *model;
   char *role; /* usually "assistant" */
   aimee_block_t *content;
   int n_content;
   aimee_stop_reason_t stop_reason;
   char *error_message;   /* typed provider failure; NULL on ordinary responses */
   char *raw_stop_reason; /* provider-specific string (e.g. "stop_sequence") */
   long usage_in;
   long usage_out;
   long usage_cache_read;
   long usage_cache_write;
   long usage_reasoning;
   struct cJSON *raw;                /* sidecar */
   aimee_refusal_details_t *refusal; /* owned terminal diagnostic; NULL when absent */
   int usage_unobserved; /* No provider usage observation; numeric storage must be zero. */
} aimee_response_t;

/* ---- streaming: a SEPARATE IR surface (event stream, not one object) ----
 * backend.parse_sse emits these; frontend.render_sse consumes them. Ordering and
 * block_id linkage are significant. Size caps are enforced by the delta producer. */
typedef enum
{
   AIMEE_DELTA_TURN_START = 0,
   AIMEE_DELTA_BLOCK_START, /* kind + block_id */
   AIMEE_DELTA_BLOCK_DELTA, /* block_id + kind + incremental payload */
   AIMEE_DELTA_BLOCK_STOP,  /* block_id */
   AIMEE_DELTA_TURN_STOP,   /* stop_reason + usage */
   AIMEE_DELTA_ERROR
} aimee_delta_type_t;

/* Borrowed canonical tool identity for policy views; no native projection. */
typedef struct
{
   const char *name;
   const char *namespace_name;
} aimee_tool_identity_view_t;

typedef struct
{
   aimee_delta_type_t type;
   int block_id;
   aimee_block_type_t kind;
   int message_group; /* Optional authored text grouping, BLOCK_START only. */
   /* BLOCK_START of a tool_use carries the id + name (opaque, verbatim). */
   const char *tool_id;
   const char *tool_name;
   const char *text_delta;
   const char *signature_delta;
   /* Complete opaque reasoning fields, never concatenated fragments. May arrive
    * at block start or later; repeated values must be identical. */
   const char *thinking_id;
   const char *thinking_encrypted_content;
   const char *redacted_content;
   const char *tool_namespace;
   const struct cJSON *tool_input;  /* BLOCK_START initial arguments; borrowed */
   const char *tool_args_delta;     /* BLOCK_DELTA for tool_use argument JSON fragment */
   aimee_stop_reason_t stop_reason; /* TURN_STOP */
   const char *raw_stop_reason;     /* native terminal spelling, borrowed */
   long usage_in, usage_out;        /* TURN_STOP */
   long usage_cache_read, usage_cache_write, usage_reasoning;
   const char *error_message;              /* ERROR */
   const aimee_refusal_details_t *refusal; /* TURN_STOP diagnostic, borrowed */
   int usage_unobserved; /* TURN_STOP: omit unknown usage rather than report zero. */
} aimee_delta_t;

/* Bounded owned model-delta assembly for canonical persistence/replay. This
 * consumer knows no native provider protocol. Free after success or any refusal. */
typedef struct
{
   aimee_response_t response;
   struct aimee_ir_builder_storage *storage;
   size_t capacity, bytes;
   int started, complete;
} aimee_ir_response_builder_t;
int aimee_ir_response_builder_apply(aimee_ir_response_builder_t *builder,
                                    const aimee_delta_t *delta);
/* Number of immutable closed blocks in the contiguous response prefix. */
int aimee_ir_response_builder_closed_prefix(const aimee_ir_response_builder_t *builder);
void aimee_ir_response_builder_free(aimee_ir_response_builder_t *builder);

/* ---- lifecycle ---- */
void aimee_message_free(aimee_message_t *message);
/* Deep owned typed snapshot, including provenance and exact native sidecars.
 * out must be zero-initialized or owned; success atomically replaces it and
 * failure leaves it intact. source == out is supported. No wire translation. */
int aimee_ir_request_clone(const aimee_request_t *source, aimee_request_t *out);
/* Exact owned response snapshot. Source and existing output are unchanged on
 * failure; context metadata and opaque numeric/reasoning values stay exact. */
int aimee_ir_response_clone(const aimee_response_t *source, aimee_response_t *out);
void aimee_request_free(aimee_request_t *r);
void aimee_response_free(aimee_response_t *r);
/* Stable comparison-only JSON view; opaque source values remain unchanged.
 * Duplicate member names refuse rather than acquire conflicting meanings. */
int aimee_ir_json_unique(const struct cJSON *json);
/* Exact JSON frame admission: valid UTF-8, no embedded/escaped NUL, one value,
 * and unambiguous object keys. Opaque numeric spelling is retained. Owned. */
struct cJSON *aimee_ir_json_parse_frame(const char *frame, size_t length);
struct cJSON *aimee_ir_json_canonical_copy(const struct cJSON *json);
int aimee_ir_thinking_binding_valid(const char *binding);
int aimee_ir_block_valid(const aimee_block_t *block);
/* Validate completed request content before host policy or native rendering. */
int aimee_ir_request_admit(const aimee_request_t *request);
/* Exact advertised function lookup. Names and namespaces are opaque, separate
 * identities; missing/hosted/ambiguous identities return NULL. Borrowed result. */
const aimee_tool_t *aimee_ir_find_function(const aimee_request_t *request,
                                           const char *namespace_name, const char *name);

void aimee_block_free_contents(aimee_block_t *b);

/* Set metadata on one block. Content ownership is unchanged. */
void aimee_ir_block_set_context(aimee_block_t *block, aimee_context_origin_t origin,
                                aimee_context_authority_t authority, aimee_context_trust_t trust,
                                aimee_context_sensitivity_t sensitivity, int model_visible,
                                const char *revision_domain, const char *revision_scope,
                                unsigned long long revision_epoch);

/* Egress-only owned projection: exclude hidden/restricted content and move
 * unverified system evidence to user role. Source history/profile stay exact.
 * Source and output must be distinct. The preflight preserves untouched native-byte
 * replay when no projection is needed. */
/* Visibility for model-derived views; unclassified legacy blocks remain visible. */
int aimee_ir_block_model_visible(const aimee_block_t *block);
/* Public ingress trust attenuation, preserving content and effective model
 * visibility/privacy. mode0 is user content; mode1 is the explicit legacy
 * request.system policy setting; mode2 is an untrusted system/developer message.
 * No caller capability grants platform origin or verified revision custody. */
void aimee_ir_block_attenuate_public(aimee_block_t *block, int system_mode);
int aimee_ir_request_needs_model_projection(const aimee_request_t *request);
int aimee_ir_request_project_model(const aimee_request_t *request, aimee_request_t *out);

/* Apply conservative defaults only to unspecified provenance. Explicit partial
 * context metadata remains unchanged. This is
 * metadata-only and does not dirty provider payload bytes. Returns the number
 * of blocks classified. */
int aimee_ir_classify_context(aimee_request_t *request);

/* Explicit authority-promotion policy. Model/tool/retrieval/memory content can
 * never become task instructions or policy. User content may be a task
 * instruction but never platform policy. */
int aimee_ir_context_promotion_allowed(aimee_context_origin_t origin,
                                       aimee_context_authority_t from,
                                       aimee_context_authority_t to);

/* Build a canonical assistant response carrying a single TEXT block for a
 * genuinely text-only source. Native model/harness adapters retain their typed
 * blocks instead; terminal screens cannot supply structured model history. `model` may be
 * NULL/empty. stop_reason is END_TURN. Fills caller-provided `out` (stack-ok); free with
 * aimee_response_free. Returns 0 on success, -1 on allocation failure (out is left freed/zeroed).
 */
/* Genuinely text-only request origin; structured callers retain their existing
 * IR instead. out is owned/zeroed, replaced atomically, and freed normally. */
int aimee_ir_request_from_message(aimee_request_t *out, const char *system,
                                  const aimee_message_t *message, int max_tokens,
                                  double temperature);
int aimee_ir_request_from_text(aimee_request_t *out, const char *system, const char *user,
                               int max_tokens, double temperature);
int aimee_ir_response_from_text(aimee_response_t *out, const char *text, const char *model);

/* ---- accessors used by the core stages / KB ----
 * Concatenate model-visible text of the LAST user-role message into `buf`
 * (truncating to n). Hidden/restricted blocks never enter derived query views.
 * This is the ONE shape-agnostic query extractor that replaces
 * ingress_preinject_query_from_messages' per-shape arms (which dropped non-text
 * blocks). Returns the number of chars written (excluding NUL), or 0 if none. */
size_t aimee_ir_last_user_text(const aimee_request_t *r, char *buf, size_t n);

/* Descending catalog index paths; validate the entire patch before mutation. */
int aimee_ir_tool_paths_valid(const aimee_request_t *request, const struct cJSON *paths);
int aimee_ir_remove_tool_paths(aimee_request_t *request, const struct cJSON *paths);

/* Pure response validation: 0 valid, -1 malformed; never changes source IR. */
/* A completed authored answer may contain IMAGE/DOCUMENT and inert THINKING
 * or opaque sidecars on admitted blocks. No pending tools/errors/refusal. */
int aimee_ir_response_complete_valid(const aimee_response_t *response);
/* Business-answer projection requires a successful complete text turn. */
int aimee_ir_response_complete_text_valid(const aimee_response_t *response);
int aimee_ir_response_valid(const aimee_response_t *response);
/* Admit a completed API model response, rejecting forged results/roles, duplicate
 * tool IDs, malformed blocks and invalid function arguments. Classifies provenance. */
int aimee_ir_response_admit(aimee_response_t *response);
/* Optional usage counter: absent is unchanged; malformed/negative/fractional or
 * inexact/unrepresentable values return -1 before any integer conversion. */
int aimee_ir_read_usage(const struct cJSON *usage, const char *name, long *out);

/* ---- response-side accessors (delegate/core stages read these) ----
 *
 * The request side has had aimee_ir_last_user_text since slice 0; the response
 * side had nothing, which is why every consumer still reaches for
 * parsed_response_t. These are the shape-agnostic reads that replace its
 * `content` / `is_tool_call` / `calls[]` fields.
 *
 * All three iterate blocks and are deliberately type-strict: THINKING is NEVER
 * treated as answer text, and only TEXT blocks are concatenated. That structural
 * separation is what removes the need to regex reasoning out of the text at all. */

/* Concatenate the response's TEXT blocks into `buf` (truncating to n). THINKING
 * blocks are skipped: reasoning is not the answer. Returns chars written
 * (excluding NUL), 0 if none. */
size_t aimee_ir_response_text(const aimee_response_t *r, char *buf, size_t n);

/* Non-zero if the response carries at least one TOOL_USE block — the IR spelling
 * of parsed_response_t.is_tool_call. */
int aimee_ir_response_has_tool_use(const aimee_response_t *r);
/* Tool ordinals count TOOL_USE blocks only; other blocks retain their order.
 * Returned tool blocks are borrowed from the response, never projected copies. */
int aimee_ir_response_tool_count(const aimee_response_t *response);
aimee_block_t *aimee_ir_response_tool(aimee_response_t *response, int ordinal);
int aimee_ir_response_drop_tool(aimee_response_t *response, int ordinal);
/* Allocate a text projection only at a consumer that requires flat text.
 * The owned IR retains thinking, tools, media and all other block types. */
/* Owned URI for media: inline base64 plus MIME becomes a data URI. */
char *aimee_ir_media_url(const aimee_block_t *block);
char *aimee_ir_response_text_alloc(const aimee_response_t *response);

/* Concatenate the response's THINKING blocks into `buf`. The reasoning is handed
 * back rather than discarded, so callers that want to log or audit it can. */
size_t aimee_ir_response_reasoning(const aimee_response_t *r, char *buf, size_t n);

/* Canonical stop-reason name (stable string for the enum). */
const char *aimee_stop_reason_name(aimee_stop_reason_t s);
aimee_stop_reason_t aimee_stop_reason_parse(const char *canonical_name);

/* 1 if two requests are SEMANTICALLY equal: same system blocks, messages (role +
 * ordered content blocks incl. tool ids/names/args/results + cache_control),
 * tools, tool_choice, and sampling params. IGNORES provenance that legitimately
 * differs across frontends: `frontend` wire tag, the `raw` sidecar, and `model`
 * (the client's model string). This is the golden-test assertion that an
 * Anthropic-shaped turn and an OpenAI-shaped turn with identical semantics produce
 * identical IR -> identical KB input + identical backend build. */
/* Native byte equality excludes host provenance/replay metadata only. */
int aimee_ir_request_wire_equal(const aimee_request_t *a, const aimee_request_t *b);
int aimee_ir_request_equal(const aimee_request_t *a, const aimee_request_t *b);

/* ----- Request transform stage (the protocol-neutral module seam) -------------
 * A transform edits the typed IR request in place BETWEEN frontend_parse and
 * backend_build. This is where modules act ONCE, on the one IR, regardless of the
 * client wire -- replacing the per-ingress, per-wire-format sites (gw_stage_memory's
 * three arms, the economizer's gateway seams, tool policing, ...). Producer-specific
 * translation stays at the edges (frontend/backend); everything here is neutral. */

/* Edit `ir` in place; return nonzero IFF the typed fields (messages/system/tools/...)
 * were CHANGED, so the caller sets ir->mutated and a same-protocol backend
 * re-serializes instead of shipping the now-stale raw sidecar. A read-only transform
 * (e.g. measurement) returns 0. */
typedef int (*aimee_ir_transform_fn)(aimee_request_t *ir, void *ud);

typedef struct
{
   const char *name; /* stable id, for ordering + trace */
   aimee_ir_transform_fn fn;
   void *ud;
   int enabled; /* 0 -> the slot is skipped */
} aimee_ir_transform_t;

/* Run `stages` (length n) over `ir` in catalog order; skips disabled / empty-name /
 * NULL-fn slots. Sets ir->mutated if any transform reports a change. NULL-safe. */
void aimee_ir_run_transforms(aimee_request_t *ir, const aimee_ir_transform_t *stages, size_t n);

/* The SINGLE request-transform stage every ingress funnels through: all three
 * aimee_ir_serve build paths call this after frontend_parse and before
 * backend_build. Modules are registered HERE (one place, in aimee_ir_serve.c so the
 * pure IR core stays free of module + config linkage) as they are ported onto the
 * IR. `memory_enabled` is the caller-resolved gw_stage_memory toggle (config-store
 * modules.memory -> env default), injected here exactly like the legacy stage
 * catalogs so the transform stays config-free. */
void aimee_ir_apply_request_stages(aimee_request_t *ir, int memory_enabled);

/* Detect unproductive tool-use loops from the protocol-neutral transcript and
 * append a neutral recovery suggestion at the threshold where it first becomes
 * useful.  This is applied to every normal OpenAI/Anthropic session. */
/* Prepend persona instructions to the first user message, once per conversation.
 * Returns 1 when placed, 0 otherwise. */
int aimee_ir_prepend_persona_instructions(aimee_request_t *request, const char *instructions);

int aimee_ir_stage_session_assist(aimee_request_t *request, void *unused);

typedef struct
{
   int tool_calls;
   int redundant_tool_calls;
   char intervention[40];
} aimee_ir_session_metrics_t;

/* Cumulative metrics derivable from an ordinary API transcript.  Evaluation
 * quality remains external; these are runtime cost/behavior observations. */
void aimee_ir_session_measure(const aimee_request_t *request, aimee_ir_session_metrics_t *out);

#endif /* DEC_AIMEE_IR_H */
