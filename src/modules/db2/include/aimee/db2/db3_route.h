#ifndef AIMEE_DB2_DB3_ROUTE_H
#define AIMEE_DB2_DB3_ROUTE_H 1

#include <stddef.h>
#include <stdint.h>

#include <aimee/db2/db3_contract.h>

typedef enum
{
   AIMEE_DB3_OK = 0,
   AIMEE_DB3_INVALID_REQUEST,
   AIMEE_DB3_UNAVAILABLE,
   AIMEE_DB3_PROVIDER_FAILURE,
   AIMEE_DB3_INVALID_RESPONSE,
   AIMEE_DB3_INTERNAL
} aimee_db3_result_t;

typedef enum
{
   AIMEE_DB3_ROUTE_DEFAULT_PGVECTOR = 0,
   AIMEE_DB3_ROUTE_EXTERNAL,
   AIMEE_DB3_ROUTE_EXPLICIT_FALLBACK
} aimee_db3_route_kind_t;

typedef enum
{
   AIMEE_DB3_APPLY_UPSERT = 1,
   AIMEE_DB3_APPLY_DELETE = 2,
   AIMEE_DB3_APPLY_TOMBSTONE = 3
} aimee_db3_apply_kind_t;

typedef struct
{
   int64_t point_id;
   double score;
} aimee_db3_candidate_t;

typedef struct
{
   char key[AIMEE_DB3_MAX_LABEL_KEY];
   char value[AIMEE_DB3_MAX_LABEL_VALUE];
} aimee_db3_exact_label_t;

typedef struct
{
   uint64_t request_id;
   uint64_t required_generation;
   char workspace[AIMEE_DB3_MAX_SCOPE];
   char project[AIMEE_DB3_MAX_SCOPE];
   char record_type[AIMEE_DB3_MAX_RECORD_TYPE];
   uint32_t dimension;
   uint32_t top_k;
   float vector[AIMEE_DB3_MAX_DIM];
   /* Exact-match filters beyond the three fixed scope fields (wire v2).
    *
    * Keys must be strictly ascending, which makes one filter set have exactly
    * one encoding. A request with none encodes byte-identically to v1, so a v1
    * peer reads it unchanged.
    *
    * Every filter must be honoured. A provider that cannot apply one must fail
    * the search rather than answer a wider question than was asked. */
   uint32_t filter_count;
   aimee_db3_exact_label_t filters[AIMEE_DB3_MAX_LABELS];
} aimee_db3_search_request_t;

typedef struct
{
   uint64_t request_id;
   uint64_t generation;
   uint32_t count;
   aimee_db3_candidate_t candidates[AIMEE_DB3_MAX_TOP_K];
} aimee_db3_search_reply_t;

typedef struct
{
   uint64_t operation_id;
   uint64_t generation;
   int64_t point_id;
   aimee_db3_apply_kind_t kind;
   char collection[AIMEE_DB3_MAX_COLLECTION];
   uint32_t dimension;
   float vector[AIMEE_DB3_MAX_DIM];
   uint32_t label_count;
   aimee_db3_exact_label_t labels[AIMEE_DB3_MAX_LABELS];
} aimee_db3_apply_t;

typedef int (*aimee_db3_search_fn)(void *context, const aimee_db3_search_request_t *request,
                                   aimee_db3_search_reply_t *reply);
typedef int (*aimee_db3_candidate_authorize_fn)(void *context, const char *workspace,
                                                const char *project, int64_t point_id);

typedef struct
{
   aimee_db3_search_fn internal_pgvector_search;
   void *internal_context;
   aimee_db3_search_fn external_search;
   void *external_context;
   aimee_db3_candidate_authorize_fn authorize_candidate;
   void *authorize_context;
   uint32_t selected_principal;
   int selected_ready;
   int fallback_enabled;
} aimee_db3_route_t;

typedef struct
{
   aimee_db3_result_t result;
   aimee_db3_route_kind_t route;
   uint32_t selected_principal;
   int external_error;
   aimee_db3_search_reply_t reply;
} aimee_db3_search_outcome_t;

int aimee_db3_search_request_validate(const aimee_db3_search_request_t *request);
int aimee_db3_search_reply_validate(const aimee_db3_search_request_t *request,
                                    const aimee_db3_search_reply_t *reply);
int aimee_db3_apply_validate(const aimee_db3_apply_t *apply);

int aimee_db3_search_request_encode(const aimee_db3_search_request_t *request, uint8_t *output,
                                    size_t capacity, size_t *length);
int aimee_db3_search_request_decode(const uint8_t *input, size_t length,
                                    aimee_db3_search_request_t *request);
int aimee_db3_search_reply_encode(const aimee_db3_search_reply_t *reply, uint8_t *output,
                                  size_t capacity, size_t *length);
int aimee_db3_search_reply_decode(const uint8_t *input, size_t length,
                                  aimee_db3_search_reply_t *reply);
int aimee_db3_apply_encode(const aimee_db3_apply_t *apply, uint8_t *output, size_t capacity,
                           size_t *length);
int aimee_db3_apply_decode(const uint8_t *input, size_t length, aimee_db3_apply_t *apply);

int aimee_db3_route_init(aimee_db3_route_t *route, aimee_db3_search_fn internal_pgvector_search,
                         void *internal_context,
                         aimee_db3_candidate_authorize_fn authorize_candidate,
                         void *authorize_context);
/* principal must be nonzero. Use aimee_db3_route_clear() to deselect the external provider. */
int aimee_db3_route_select(aimee_db3_route_t *route, uint32_t principal, int ready,
                           int fallback_enabled, aimee_db3_search_fn external_search,
                           void *external_context);
void aimee_db3_route_clear(aimee_db3_route_t *route);
aimee_db3_result_t aimee_db3_memory_candidates_search(aimee_db3_route_t *route,
                                                      const aimee_db3_search_request_t *request,
                                                      aimee_db3_search_outcome_t *outcome);

#endif /* AIMEE_DB2_DB3_ROUTE_H */
