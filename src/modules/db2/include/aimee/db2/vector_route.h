#ifndef AIMEE_DB2_VECTOR_ROUTE_H
#define AIMEE_DB2_VECTOR_ROUTE_H 1

#include <stddef.h>
#include <stdint.h>

#include <aimee/db2/vector_contract.h>

typedef enum
{
   AIMEE_VECTOR_OK = 0,
   AIMEE_VECTOR_INVALID_REQUEST,
   AIMEE_VECTOR_UNAVAILABLE,
   AIMEE_VECTOR_PROVIDER_FAILURE,
   AIMEE_VECTOR_INVALID_RESPONSE,
   AIMEE_VECTOR_INTERNAL
} aimee_vector_result_t;

typedef enum
{
   AIMEE_VECTOR_ROUTE_DEFAULT_PGVECTOR = 0,
   AIMEE_VECTOR_ROUTE_EXTERNAL,
   AIMEE_VECTOR_ROUTE_EXPLICIT_FALLBACK
} aimee_vector_route_kind_t;

typedef enum
{
   AIMEE_VECTOR_APPLY_UPSERT = 1,
   AIMEE_VECTOR_APPLY_DELETE = 2,
   AIMEE_VECTOR_APPLY_TOMBSTONE = 3
} aimee_vector_apply_kind_t;

typedef struct
{
   int64_t point_id;
   double score;
} aimee_vector_candidate_t;

/* One predicate about one label.
 *
 * `values` holds `value_count` pointers, all owned by the caller. A filter is
 * satisfied when the point's values for `key` relate to these as `op` says:
 * EQ and NE take exactly one value, IN takes one or more and matches if ANY of
 * the point's values for that key is in the set.
 *
 * IN over a MULTI-VALUED label is what removes the need for OR. Scope
 * visibility is a disjunction in SQL -- active project, active workspace,
 * global, legacy-untagged -- and a point that carries each scope it belongs to
 * as a separate value of one key turns that whole disjunction into a single
 * set-membership question. */
typedef struct
{
   uint8_t op; /* AIMEE_VECTOR_FILTER_EQ, _NE or _IN */
   const char *key;
   const char *const *values;
   size_t value_count;
} aimee_vector_filter_t;

typedef struct
{
   uint64_t request_id;
   uint64_t required_generation;
   char workspace[AIMEE_VECTOR_MAX_SCOPE];
   char project[AIMEE_VECTOR_MAX_SCOPE];
   char record_type[AIMEE_VECTOR_MAX_RECORD_TYPE];
   /* Which vector column to rank against, when a row holds more than one.
    *
    * Empty means the collection the record_type implies, which is every search
    * that has only one vector per row. Apply has always named a collection;
    * this is search catching up, and it is what `which_vec` becomes.
    *
    * Version 2 of the request. Empty collection and no filters encode as
    * version 1, so a provider that only speaks version 1 still receives every
    * request it could have served before. */
   char collection[AIMEE_VECTOR_MAX_COLLECTION];
   /* `filter_count` predicates, all conjoined, owned by the caller.
    *
    * Caller-owned for the same reason as the vector: 16 filters of 256 values
    * of 256 bytes embedded would be a megabyte of struct. */
   const aimee_vector_filter_t *filters;
   size_t filter_count;
   uint32_t dimension;
   uint32_t top_k;
   /* `dimension` floats, owned by the caller.
    *
    * A pointer rather than an array: an embedded vector makes the struct's SIZE
    * the dimension ceiling, so supporting the 16k and 32k dimensions this
    * contract exists for would make every one of these 64 KB and 128 KB. The
    * codec allocates nothing -- encode reads through this, and decode writes
    * into a buffer the caller supplies. */
   const float *vector;
} aimee_vector_search_request_t;

typedef struct
{
   uint64_t request_id;
   uint64_t generation;
   uint32_t count;
   aimee_vector_candidate_t candidates[AIMEE_VECTOR_MAX_TOP_K];
} aimee_vector_search_reply_t;

typedef struct
{
   char key[AIMEE_VECTOR_MAX_LABEL_KEY];
   char value[AIMEE_VECTOR_MAX_LABEL_VALUE];
} aimee_vector_exact_label_t;

typedef struct
{
   uint64_t operation_id;
   uint64_t generation;
   int64_t point_id;
   aimee_vector_apply_kind_t kind;
   char collection[AIMEE_VECTOR_MAX_COLLECTION];
   uint32_t dimension;
   /* `dimension` floats, owned by the caller. See the search request above.
    * NULL is correct for a delete or a tombstone, which carry no vector. */
   const float *vector;
   uint32_t label_count;
   aimee_vector_exact_label_t labels[AIMEE_VECTOR_MAX_LABELS];
} aimee_vector_apply_t;

typedef int (*aimee_vector_search_fn)(void *context, const aimee_vector_search_request_t *request,
                                      aimee_vector_search_reply_t *reply);
typedef int (*aimee_vector_candidate_authorize_fn)(void *context, const char *workspace,
                                                   const char *project, int64_t point_id);

/* How many provider attachments this route holds evidence for.
 *
 * NOT a protocol limit, and deliberately not in the catalogue: the wire places
 * no bound on how many providers a deployment attaches, and putting one there
 * would make a ninth provider protocol-invalid, which is not true. This is a
 * fixed-array implementation choosing a bound it can state, and observing past
 * it is refused as "registry full" rather than as a malformed announcement.
 *
 * The Go router uses a map and has no such bound. If a deployment ever needs
 * more than this, the answer is to raise it here, not to reinterpret it as a
 * limit on what a provider may do. */
#define AIMEE_VECTOR_MAX_PROVIDERS 8u

/* What a provider announced it can do.
 *
 * `operations`, `metrics` and `filters` are bit sets over the
 * AIMEE_VECTOR_OPERATION_*, _METRIC_* and _FILTER_EXACT constants, which come
 * from the protocol catalogue so that this and the Go implementation cannot
 * disagree about what a bit means. */
typedef struct
{
   uint64_t generation;
   uint32_t operations;
   uint32_t metrics;
   uint32_t filters;
   uint32_t max_dimension;
   uint32_t max_batch;
   uint32_t max_top_k;
   int ready;
} aimee_vector_capabilities_t;

/* 0 when the announcement is internally consistent, -1 otherwise. Decode applies
 * this, so a decoded value is always valid; it is public for a caller building
 * an announcement rather than receiving one. */
int aimee_vector_capabilities_validate(const aimee_vector_capabilities_t *capabilities);

/* Exactly AIMEE_VECTOR_CAPABILITIES_HEADER bytes in, one announcement out.
 * Returns 0, or -1 for any length, magic, version, unknown bit, or reserved
 * field this build does not understand. */
int aimee_vector_capabilities_decode(const uint8_t *bytes, size_t length,
                                     aimee_vector_capabilities_t *out);

/* Writes AIMEE_VECTOR_CAPABILITIES_HEADER bytes, or -1 if the buffer is too
 * small or the announcement does not validate. */
int aimee_vector_capabilities_encode(const aimee_vector_capabilities_t *capabilities, uint8_t *out,
                                     size_t capacity, size_t *written);

/* One provider attachment's live evidence. */
typedef struct
{
   uint32_t principal;
   uint32_t handle;
   uint64_t sequence;
   aimee_vector_capabilities_t capabilities;
} aimee_vector_provider_t;

typedef struct
{
   aimee_vector_search_fn internal_pgvector_search;
   void *internal_context;
   aimee_vector_search_fn external_search;
   void *external_context;
   aimee_vector_candidate_authorize_fn authorize_candidate;
   void *authorize_context;
   uint32_t selected_principal;
   int selected_ready;
   int fallback_enabled;
   /* Live provider evidence, and whether a control decision pinned the
    * selection. An explicit route stays pinned and therefore fails closed if
    * its provider disappears; an automatic one advances to the next eligible
    * provider, or back to pgvector when none remains. */
   aimee_vector_provider_t providers[AIMEE_VECTOR_MAX_PROVIDERS];
   size_t provider_count;
   int selection_explicit;
} aimee_vector_route_t;

typedef struct
{
   aimee_vector_result_t result;
   aimee_vector_route_kind_t route;
   uint32_t selected_principal;
   int external_error;
   aimee_vector_search_reply_t reply;
} aimee_vector_search_outcome_t;

/* The filters a DB2 search call actually has, as opposed to the three DB3 v1 can
 * carry.
 *
 * This type exists so that a filter with nowhere to go is a COMPILE-TIME
 * decision and a run-time refusal, rather than an argument someone forgot to
 * pass on. Every field below that DB3 v1 cannot express must be empty; supplying
 * one is refused, and the request is not built.
 *
 * The refusal matters more than it looks. A request built without a filter is
 * well-formed: the provider answers it, scores it and ranks it, and the result
 * is indistinguishable from a correct one. It returns MORE rows, all plausible.
 * Nothing anywhere raises an error. That is why this is checked where the
 * request is made and not where it is read. */
typedef struct
{
   /* Carried by DB3 v1. */
   const char *workspace;   /* NULL or "" for no filter */
   const char *project;     /* NULL or "" for no filter */
   const char *record_type; /* required */

   /* NOT carried by DB3 v1. Each must be empty; each is refused if it is not.
    *
    * exclude_project is negation, and kinds is set membership: both are a filter
    * language, which is more than this contract should answer at v1. rank_column
    * names which of several vector columns on one row to rank against, and needs
    * search to carry the collection that apply already names. labels need the
    * exact-label filter apply already has and search does not, which is the same
    * asymmetry. All four are named here so an adapter cannot quietly not have
    * them. */
   const char *exclude_project;
   const char *const *kinds;
   size_t kind_count;
   const char *rank_column;
   const char *const *label_keys;
   const char *const *label_values;
   size_t label_count;

   /* The two predicates the SQL applies and no caller passes, now expressible.
    *
    * They are still DECLARED rather than inferred, because an adapter that does
    * not know its query depends on them is an adapter that will drop them. The
    * difference is that declaring them now produces a request that carries them
    * instead of a refusal.
    *
    * scope_membership: visibility decided by rows in memory_scopes and
    * memory_workspaces -- active project, active workspace, global, '_shared',
    * and the ABSENCE of any scope row meaning legacy-untagged. Carried as one
    * IN over the multi-valued `visibility` label, which is why the four-way
    * disjunction needs no OR. `visibility` names the label; the values are the
    * set the caller is allowed to see.
    *
    * current_generation: the point's generation, which is fixed when the point
    * is written and therefore a label written once. Carried as one EQ, with the
    * value the caller read from projects. Nothing is relabelled when a project
    * is re-ingested; the next search simply asks for the new generation.
    *
    * Both were refusals until the wire could carry them. Dropping the first
    * returns too FEW rows and the short answer looks complete; dropping the
    * second returns superseded generations ranked among the current ones. */
   const char *const *visibility;
   size_t visibility_count;
   const char *current_generation;
} aimee_vector_search_filters_t;

/* Build a search request, or refuse.
 *
 * Returns 0 having filled `request`, or -1 having filled nothing.
 *
 * Every filter this type can express now has somewhere to go, so -1 means the
 * request is invalid or something does not FIT -- a value over its bound, a set
 * over the ceiling, more predicates than `predicate_capacity`. It no longer
 * means "this wire cannot ask that", because it can.
 *
 * `predicates` receives one entry per filter and is the caller's, like
 * `vector`: both must outlive the request, and the builder allocates nothing.
 * `predicate_capacity` should be AIMEE_VECTOR_MAX_FILTERS unless the caller knows
 * it passes fewer. */
int aimee_vector_search_request_build(const aimee_vector_search_filters_t *filters,
                                      uint64_t request_id, uint64_t required_generation,
                                      const float *vector, uint32_t dimension, uint32_t top_k,
                                      aimee_vector_filter_t *predicates, size_t predicate_capacity,
                                      aimee_vector_search_request_t *request);

/* Reports whether these filters can cross DB3 v1 at all, without building.
 *
 * For a caller deciding whether to route externally before it has a request to
 * build. Same answer as the builder's, so the two cannot disagree. */
int aimee_vector_search_filters_expressible(const aimee_vector_search_filters_t *filters);

int aimee_vector_search_request_validate(const aimee_vector_search_request_t *request);
int aimee_vector_search_reply_validate(const aimee_vector_search_request_t *request,
                                       const aimee_vector_search_reply_t *reply);
int aimee_vector_apply_validate(const aimee_vector_apply_t *apply);

int aimee_vector_search_request_encode(const aimee_vector_search_request_t *request,
                                       uint8_t *output, size_t capacity, size_t *length);
/* A read-only walk over the filters in a decoded request.
 *
 * A provider iterates this; nothing allocates. Decode has already checked every
 * length and offset against the declared filter region, so a walk cannot leave
 * the message.
 *
 * `key` and `value` point into the caller's input buffer and are NOT
 * NUL-terminated: they are length-counted, because a label value may contain
 * anything a label value may contain. */
typedef struct
{
   const uint8_t *bytes;
   size_t length;
   size_t offset;
   size_t remaining;
} aimee_vector_filter_view_t;

typedef struct
{
   uint8_t op;
   const char *key;
   size_t key_length;
   size_t value_count;
   /* Set by aimee_vector_filter_value() for each value in turn. */
   size_t value_offset;
} aimee_vector_filter_entry_t;

/* Reads the next filter, or returns 0 when the walk is done. -1 on a malformed
 * region, which decode should already have refused. */
int aimee_vector_filter_next(aimee_vector_filter_view_t *view, aimee_vector_filter_entry_t *entry);

/* Reads value `index` of the entry just returned. -1 when there is no such
 * value. */
int aimee_vector_filter_value(const aimee_vector_filter_view_t *view,
                              const aimee_vector_filter_entry_t *entry, size_t index,
                              const char **value, size_t *value_length);

/* Decode into `request`, writing the vector into `vector_out`.
 *
 * The buffer is the caller's because the struct no longer carries one, and
 * because a codec that allocates is a codec whose failures are somebody else's
 * to free. `vector_capacity` counts floats; a request whose dimension exceeds
 * it is REFUSED rather than truncated -- a short read here would be a vector
 * silently missing its tail, which scores as a perfectly ordinary result. */
int aimee_vector_search_request_decode(const uint8_t *input, size_t length,
                                       aimee_vector_search_request_t *request, float *vector_out,
                                       size_t vector_capacity,
                                       aimee_vector_filter_view_t *filters_out);
int aimee_vector_search_reply_encode(const aimee_vector_search_reply_t *reply, uint8_t *output,
                                     size_t capacity, size_t *length);
int aimee_vector_search_reply_decode(const uint8_t *input, size_t length,
                                     aimee_vector_search_reply_t *reply);
int aimee_vector_apply_encode(const aimee_vector_apply_t *apply, uint8_t *output, size_t capacity,
                              size_t *length);
/* Decode into `apply`, writing the vector into `vector_out`. See the search
 * request decode above. A delete or a tombstone carries no vector, so a NULL
 * buffer is accepted for those and refused for an upsert. */
int aimee_vector_apply_decode(const uint8_t *input, size_t length, aimee_vector_apply_t *apply,
                              float *vector_out, size_t vector_capacity);

int aimee_vector_route_init(aimee_vector_route_t *route,
                            aimee_vector_search_fn internal_pgvector_search, void *internal_context,
                            aimee_vector_candidate_authorize_fn authorize_candidate,
                            void *authorize_context);
/* principal must be nonzero. Use aimee_vector_route_clear() to deselect the external provider. */
/* Record one provider attachment's announcement and re-derive the automatic
 * selection. Returns 0 when the evidence was recorded, -1 when it is malformed
 * or stale (same attachment, non-advancing sequence) or the registry is full.
 *
 * `principal` and `handle` come from the BUS FRAME, never from the provider's
 * payload: a provider that could name its own principal could name someone
 * else's. `sequence` is monotonic within one attachment; a new handle is a new
 * attachment and may restart it.
 *
 * A search transport must have been installed for a selection to be usable --
 * see aimee_vector_route_set_transport. */
int aimee_vector_route_observe_capabilities(aimee_vector_route_t *route, uint32_t principal,
                                            uint32_t handle, uint64_t sequence,
                                            const aimee_vector_capabilities_t *capabilities);

/* Drop one attachment's evidence. Returns 1 if it was present, 0 otherwise. */
int aimee_vector_route_remove_provider(aimee_vector_route_t *route, uint32_t principal,
                                       uint32_t handle);

/* How a search reaches whichever provider is selected. Installed once; the
 * transport reads route->selected_principal to know who it is addressing, so
 * changing the selection does not change the transport. */
int aimee_vector_route_set_transport(aimee_vector_route_t *route,
                                     aimee_vector_search_fn external_search, void *external_context,
                                     int fallback_enabled);

int aimee_vector_route_select(aimee_vector_route_t *route, uint32_t principal, int ready,
                              int fallback_enabled, aimee_vector_search_fn external_search,
                              void *external_context);
void aimee_vector_route_clear(aimee_vector_route_t *route);
aimee_vector_result_t
aimee_vector_memory_candidates_search(aimee_vector_route_t *route,
                                      const aimee_vector_search_request_t *request,
                                      aimee_vector_search_outcome_t *outcome);

#endif /* AIMEE_DB2_VECTOR_ROUTE_H */
