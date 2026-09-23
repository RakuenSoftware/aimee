#include <stdlib.h>
#include <string.h>
#include "wire_fence.h"
#include "request_context.h"
#include "modules/economizer/economizer_module_client.h"

/* Lean clients may have no HTTP context. Deployment limits still apply, and a
 * present limit must never bypass admission because the module is absent. */
extern const request_context_t *request_context_get(void) __attribute__((weak));
extern int ingress_preinject_revalidate_sources(void) __attribute__((weak));
extern econ_request_budget_result_t econ_module_request_budget(unsigned, const void *, size_t,
                                                               const char *) __attribute__((weak));
extern econ_request_budget_result_t
econ_module_request_budget_with_policy(unsigned, const void *, size_t, const char *, const char *)
    __attribute__((weak));
extern int server_error_kind_http_status(const char *) __attribute__((weak));
static _Thread_local const char *last_error;
const char *wire_fence_last_error(void)
{
   return last_error ? last_error : "request_pipeline_unavailable";
}
int wire_fence_error_http_status(const char *error)
{
   if (!error)
      return 502;
   if (!strcmp(error, "request_budget_exceeded"))
      return 413;
   if (!strcmp(error, "request_budget_invalid") || !strcmp(error, "token_count_unavailable"))
      return 400;
   if (!strcmp(error, "request_budget_unavailable") ||
       !strcmp(error, "request_budget_policy_invalid"))
      return 503;
   int classified = server_error_kind_http_status ? server_error_kind_http_status(error) : 0;
   return classified ? classified : 502;
}
const char *wire_fence_error_type(const char *error)
{
   return wire_fence_error_http_status(error) == 502 ? "upstream_error" : error;
}

struct wire_fence
{
   wire_fence_route_t route;
   uint8_t *data;
   size_t len;
};

static int route_valid(wire_fence_route_t route)
{
   return route == WIRE_FENCE_OPENAI_CHAT || route == WIRE_FENCE_OPENAI_RESPONSES ||
          route == WIRE_FENCE_ANTHROPIC_MESSAGES;
}

/* THE FENCE HAS NO CANDIDATE PATH, and that is what makes it safe.
 *
 * This used to also assert that the proof registry was signed and EMPTY, i.e.
 * that no transform was authorized to alter the wire body. That assertion moved
 * with the proof planner into the Go economizer module, and re-asking it here
 * would mean a bus round trip on every provider call to learn something this
 * file already guarantees structurally: wire_fence_select only ever returns the
 * PRISTINE bytes, either passed through or frozen into a snapshot. There is no
 * branch that can emit anything else.
 *
 * IF YOU ADD A CANDIDATE PATH HERE, that guarantee is gone and you must gate it
 * on an authorization decision from the economizer module. Do not add one and
 * assume this fence still protects you. */
int wire_fence_create(wire_fence_route_t route, const void *pristine, size_t pristine_len,
                      wire_fence_t **out)
{
   if (!out)
      return -1;
   *out = NULL;
   if (!route_valid(route) || (!pristine && pristine_len != 0))
      return -1;
   if (pristine_len == SIZE_MAX)
      return -1;

   wire_fence_t *snapshot = calloc(1, sizeof(*snapshot));
   if (!snapshot)
      return -1;
   snapshot->data = malloc(pristine_len + 1);
   if (!snapshot->data)
   {
      free(snapshot);
      return -1;
   }
   if (pristine_len)
      memcpy(snapshot->data, pristine, pristine_len);
   snapshot->data[pristine_len] = '\0';
   snapshot->route = route;
   snapshot->len = pristine_len;
   *out = snapshot;
   return 0;
}

int wire_fence_revalidate_sources(void)
{
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (context && (context->context_refused || (context->memory_source_release[0] &&
                                                (!ingress_preinject_revalidate_sources ||
                                                 ingress_preinject_revalidate_sources() != 0))))
   {
      last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
      return WIRE_FENCE_CONTEXT_REFUSED;
   }
   return 0;
}

int wire_fence_select(int proof_gated, wire_fence_route_t route, const void *pristine,
                      size_t pristine_len, wire_fence_t **snapshot, wire_fence_bytes_t *selected)
{
   last_error = "request_pipeline_unavailable";
   if (!snapshot || !selected || (!pristine && pristine_len != 0) || !route_valid(route))
      return -1;
   *snapshot = NULL;
   selected->data = NULL;
   selected->len = 0;
   const request_context_t *context = request_context_get ? request_context_get() : NULL;
   if (context && context->context_refused)
   {
      last_error = context->context_refusal_kind[0] ? context->context_refusal_kind : "unavailable";
      return WIRE_FENCE_CONTEXT_REFUSED;
   }
   /* Deployment-owned metadata is forwarded unchanged. Go owns validation and
    * intersection with the caller's limit. Empty/unset means no operator cap;
    * literal zero is expressed in the versioned JSON object. */
   const char *policy = getenv("AIMEE_PROVIDER_CONTEXT_LIMITS");
   if (policy && !*policy)
      policy = NULL;
   if (policy || (context && context->request_budget_present))
   {
      const char *limits =
          context && context->request_budget_present > 0 ? context->request_budget_limits : NULL;
      econ_request_budget_result_t result =
          context && context->request_budget_present < 0 ? ECON_REQUEST_BUDGET_INVALID
          : policy                                       ? (econ_module_request_budget_with_policy
                                                                ? econ_module_request_budget_with_policy((unsigned)route, pristine,
                                                                                                         pristine_len, limits, policy)
                                                                : ECON_REQUEST_BUDGET_UNAVAILABLE)
          : econ_module_request_budget
              ? econ_module_request_budget((unsigned)route, pristine, pristine_len, limits)
              : ECON_REQUEST_BUDGET_UNAVAILABLE;
      if (result != ECON_REQUEST_BUDGET_ADMITTED)
      {
         last_error = result == ECON_REQUEST_BUDGET_INVALID              ? "request_budget_invalid"
                      : result == ECON_REQUEST_BUDGET_OVERFLOW           ? "request_budget_exceeded"
                      : result == ECON_REQUEST_BUDGET_TOKENS_UNAVAILABLE ? "token_count_unavailable"
                      : result == ECON_REQUEST_BUDGET_POLICY_INVALID
                          ? "request_budget_policy_invalid"
                          : "request_budget_unavailable";
         return -1;
      }
   }
   if (wire_fence_revalidate_sources() != 0)
      return WIRE_FENCE_CONTEXT_REFUSED;
   if (!proof_gated)
   {
      selected->data = pristine;
      selected->len = pristine_len;
      return 0;
   }
   if (wire_fence_create(route, pristine, pristine_len, snapshot) != 0)
      return -1;
   *selected = wire_fence_bytes(*snapshot);
   return 0;
}

wire_fence_route_t econ_wire_snapshot_route(const wire_fence_t *snapshot)
{
   return snapshot ? snapshot->route : 0;
}

wire_fence_bytes_t wire_fence_bytes(const wire_fence_t *snapshot)
{
   wire_fence_bytes_t bytes = {0};
   if (snapshot)
   {
      bytes.data = snapshot->data;
      bytes.len = snapshot->len;
   }
   return bytes;
}

void wire_fence_destroy(wire_fence_t *snapshot)
{
   if (!snapshot)
      return;
   free(snapshot->data);
   snapshot->data = NULL;
   snapshot->len = 0;
   free(snapshot);
}
