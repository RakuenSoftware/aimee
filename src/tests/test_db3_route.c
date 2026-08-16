#include <aimee/db2/db3_route.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(AIMEE_DB3_EVENT_CAPABILITIES == 0x80030001u, "DB3 capability event drift");
_Static_assert(AIMEE_DB3_EVENT_APPLY == 0x80030002u, "DB3 apply event drift");
_Static_assert(AIMEE_DB3_EVENT_APPLIED == 0x80030003u, "DB3 applied event drift");
_Static_assert(AIMEE_DB3_EVENT_SEARCH == 0x80030004u, "DB3 search event drift");
_Static_assert(AIMEE_DB3_EVENT_ROUTE == 0x80030005u, "DB3 route event drift");

typedef struct
{
   int calls;
   int mode;
   int64_t first_id;
} search_state_t;

typedef struct
{
   int calls;
   int reject_id;
   int fail;
} auth_state_t;

static int search(void *context, const aimee_db3_search_request_t *request,
                  aimee_db3_search_reply_t *reply)
{
   search_state_t *state = context;
   state->calls++;
   if (state->mode == 1)
      return -1;
   reply->request_id = request->request_id;
   reply->generation = request->required_generation;
   reply->count = 3;
   for (uint32_t i = 0; i < reply->count; ++i)
   {
      reply->candidates[i].point_id = state->first_id + (int64_t)i;
      reply->candidates[i].score = 0.9 - 0.1 * (double)i;
   }
   if (state->mode == 2)
      reply->candidates[0].point_id = 0;
   else if (state->mode == 3)
      reply->candidates[0].score = NAN;
   else if (state->mode == 4)
      reply->candidates[1].point_id = reply->candidates[0].point_id;
   else if (state->mode == 5)
      reply->generation++;
   return 0;
}

static int authorize(void *context, const char *workspace, const char *project, int64_t point_id)
{
   auth_state_t *state = context;
   state->calls++;
   assert(strcmp(workspace, "workspace-a") == 0);
   assert(strcmp(project, "project-a") == 0);
   if (state->fail)
      return -1;
   return point_id != state->reject_id;
}

static aimee_db3_search_request_t request(void)
{
   aimee_db3_search_request_t value = {0};
   value.request_id = 91;
   value.required_generation = 7;
   strcpy(value.workspace, "workspace-a");
   strcpy(value.project, "project-a");
   strcpy(value.record_type, "memory");
   value.dimension = 3;
   value.top_k = 3;
   value.vector[0] = 0.25f;
   value.vector[1] = -0.5f;
   value.vector[2] = 0.75f;
   return value;
}

static void test_default_external_and_authorization(void)
{
   search_state_t internal = {.first_id = 10}, external = {.first_id = 20};
   auth_state_t auth = {.reject_id = 11};
   aimee_db3_route_t route;
   assert(aimee_db3_route_init(&route, search, &internal, authorize, &auth) == 0);
   aimee_db3_search_request_t req = request();
   aimee_db3_search_outcome_t out;

   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_OK);
   assert(out.route == AIMEE_DB3_ROUTE_DEFAULT_PGVECTOR && out.selected_principal == 0);
   assert(internal.calls == 1 && external.calls == 0 && out.reply.count == 2);
   assert(out.reply.candidates[0].point_id == 10 && out.reply.candidates[1].point_id == 12);

   auth.reject_id = 21;
   assert(aimee_db3_route_select(&route, 1001, 1, 0, search, &external) == 0);
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_OK);
   assert(out.route == AIMEE_DB3_ROUTE_EXTERNAL && out.selected_principal == 1001);
   assert(internal.calls == 1 && external.calls == 1 && out.reply.count == 2);
   assert(out.reply.candidates[0].point_id == 20 && out.reply.candidates[1].point_id == 22);

   aimee_db3_route_clear(&route);
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_OK);
   assert(out.route == AIMEE_DB3_ROUTE_DEFAULT_PGVECTOR && internal.calls == 2);
}

static void test_fail_closed_and_explicit_fallback(void)
{
   search_state_t internal = {.first_id = 10}, external = {.first_id = 20, .mode = 1};
   auth_state_t auth = {.reject_id = -1};
   aimee_db3_route_t route;
   aimee_db3_search_request_t req = request();
   aimee_db3_search_outcome_t out;
   assert(aimee_db3_route_init(&route, search, &internal, authorize, &auth) == 0);

   assert(aimee_db3_route_select(&route, 1001, 0, 0, search, &external) == 0);
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_UNAVAILABLE);
   assert(out.route == AIMEE_DB3_ROUTE_EXTERNAL && out.external_error == AIMEE_DB3_UNAVAILABLE);
   assert(internal.calls == 0 && external.calls == 0);

   assert(aimee_db3_route_select(&route, 1001, 1, 0, search, &external) == 0);
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_PROVIDER_FAILURE);
   assert(internal.calls == 0 && external.calls == 1);

   assert(aimee_db3_route_select(&route, 1001, 1, 1, search, &external) == 0);
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_OK);
   assert(out.route == AIMEE_DB3_ROUTE_EXPLICIT_FALLBACK);
   assert(out.external_error == AIMEE_DB3_PROVIDER_FAILURE);
   assert(internal.calls == 1 && external.calls == 2);

   external.mode = 3;
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_OK);
   assert(out.route == AIMEE_DB3_ROUTE_EXPLICIT_FALLBACK);
   assert(out.external_error == AIMEE_DB3_INVALID_RESPONSE);
   assert(internal.calls == 2 && external.calls == 3);

   route.fallback_enabled = 0;
   for (int mode = 2; mode <= 5; ++mode)
   {
      external.mode = mode;
      assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INVALID_RESPONSE);
      assert(out.reply.count == 3 || mode == 2);
   }

   external.mode = 0;
   auth.fail = 1;
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INTERNAL);
}

static void test_invalid_requests(void)
{
   search_state_t internal = {.first_id = 1};
   auth_state_t auth = {0};
   aimee_db3_route_t route;
   aimee_db3_search_outcome_t out;
   assert(aimee_db3_route_init(&route, search, &internal, authorize, &auth) == 0);
   aimee_db3_search_request_t req = request();

   req.vector[1] = INFINITY;
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INVALID_REQUEST);
   req = request();
   req.top_k = 0;
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INVALID_REQUEST);
   req = request();
   req.workspace[0] = '\0';
   req.project[0] = '\0';
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INVALID_REQUEST);
   req = request();
   memset(req.record_type, 'x', sizeof(req.record_type));
   assert(aimee_db3_memory_candidates_search(&route, &req, &out) == AIMEE_DB3_INVALID_REQUEST);
   assert(internal.calls == 0);
}

static void test_wire_codecs(void)
{
   uint8_t wire[512], mutated[512];
   size_t length = 0;
   aimee_db3_search_request_t req = request(), decoded_req;
   assert(aimee_db3_search_request_encode(&req, wire, sizeof(wire), &length) == 0);
   assert(length == 36 + strlen(req.workspace) + strlen(req.project) + strlen(req.record_type) +
                        req.dimension * sizeof(float));
   assert(aimee_db3_search_request_decode(wire, length, &decoded_req) == 0);
   assert(decoded_req.request_id == req.request_id && decoded_req.dimension == req.dimension);
   assert(memcmp(decoded_req.vector, req.vector, req.dimension * sizeof(float)) == 0);
   assert(aimee_db3_search_request_decode(wire, length - 1, &decoded_req) != 0);
   memcpy(mutated, wire, length);
   mutated[34] = 1;
   assert(aimee_db3_search_request_decode(mutated, length, &decoded_req) != 0);
   memcpy(mutated, wire, length);
   mutated[length - 1] = 0x7f;
   mutated[length - 2] = 0x80;
   mutated[length - 3] = 0;
   mutated[length - 4] = 0;
   assert(aimee_db3_search_request_decode(mutated, length, &decoded_req) != 0);

   aimee_db3_search_reply_t reply = {.request_id = req.request_id,
                                     .generation = req.required_generation,
                                     .count = 2,
                                     .candidates = {{101, 0.9}, {102, 0.8}}};
   aimee_db3_search_reply_t decoded_reply;
   assert(aimee_db3_search_reply_encode(&reply, wire, sizeof(wire), &length) == 0);
   assert(aimee_db3_search_reply_decode(wire, length, &decoded_reply) == 0);
   assert(aimee_db3_search_reply_validate(&req, &decoded_reply) == 0);
   assert(aimee_db3_search_reply_decode(wire, length - 1, &decoded_reply) != 0);
   assert(aimee_db3_search_reply_decode(wire, length, &decoded_reply) == 0);
   decoded_reply.candidates[1].point_id = 101;
   assert(aimee_db3_search_reply_validate(&req, &decoded_reply) != 0);
   assert(aimee_db3_search_reply_encode(&decoded_reply, wire, sizeof(wire), &length) != 0);

   aimee_db3_apply_t apply = {.operation_id = 1001,
                              .generation = 7,
                              .point_id = 101,
                              .kind = AIMEE_DB3_APPLY_UPSERT,
                              .collection = "memory",
                              .dimension = 3,
                              .vector = {0.1f, 0.2f, 0.3f}};
   aimee_db3_apply_t decoded_apply;
   assert(aimee_db3_apply_encode(&apply, wire, sizeof(wire), &length) == 0);
   assert(aimee_db3_apply_decode(wire, length, &decoded_apply) == 0);
   assert(decoded_apply.operation_id == apply.operation_id && decoded_apply.dimension == 3);
   assert(aimee_db3_apply_decode(wire, length - 1, &decoded_apply) != 0);
   apply.vector[0] = NAN;
   assert(aimee_db3_apply_encode(&apply, wire, sizeof(wire), &length) != 0);
   apply.vector[0] = 0.1f;
   apply.kind = AIMEE_DB3_APPLY_DELETE;
   assert(aimee_db3_apply_validate(&apply) != 0);
   apply.dimension = 0;
   assert(aimee_db3_apply_validate(&apply) == 0);

   /* Generated tests/baselines/modules/db3-wire-v1.json replays these exact
    * bytes in Go. Pin them here as the C side of the cross-language fixture. */
   const uint8_t expected_request[] = {
       0x44, 0x42, 0x33, 0x53, 0x01, 0x00, 0x24, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b, 0x00, 0x09, 0x00, 0x06, 0x00,
       0x03, 0x00, 0x02, 0x00, 0x00, 0x00, 0x77, 0x6f, 0x72, 0x6b, 0x73, 0x70, 0x61, 0x63, 0x65,
       0x2d, 0x61, 0x70, 0x72, 0x6f, 0x6a, 0x65, 0x63, 0x74, 0x2d, 0x61, 0x6d, 0x65, 0x6d, 0x6f,
       0x72, 0x79, 0x9a, 0x99, 0x99, 0x3e, 0xcd, 0xcc, 0x4c, 0x3e, 0xcd, 0xcc, 0xcc, 0x3d};
   const uint8_t expected_reply[] = {
       0x44, 0x42, 0x33, 0x52, 0x01, 0x00, 0x1c, 0x00, 0x4d, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x29, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0xee, 0x3f, 0x2a,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe8, 0x3f};
   const uint8_t expected_apply[] = {
       0x44, 0x42, 0x33, 0x41, 0x01, 0x00, 0x01, 0x00, 0xe9, 0x03, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x03, 0x00, 0x6d, 0x65, 0x6d, 0x6f, 0x72, 0x79,
       0xcd, 0xcc, 0xcc, 0x3d, 0xcd, 0xcc, 0x4c, 0x3e, 0x9a, 0x99, 0x99, 0x3e};
   aimee_db3_search_request_t fixture_request = {.request_id = 77,
                                                 .required_generation = 7,
                                                 .workspace = "workspace-a",
                                                 .project = "project-a",
                                                 .record_type = "memory",
                                                 .dimension = 3,
                                                 .top_k = 2,
                                                 .vector = {0.3f, 0.2f, 0.1f}};
   assert(aimee_db3_search_request_encode(&fixture_request, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_request) && memcmp(wire, expected_request, length) == 0);
   aimee_db3_search_reply_t fixture_reply = {
       .request_id = 77, .generation = 7, .count = 2, .candidates = {{41, 0.95}, {42, 0.75}}};
   assert(aimee_db3_search_reply_encode(&fixture_reply, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_reply) && memcmp(wire, expected_reply, length) == 0);
   aimee_db3_apply_t fixture_apply = {.operation_id = 1001,
                                      .generation = 7,
                                      .point_id = 41,
                                      .kind = AIMEE_DB3_APPLY_UPSERT,
                                      .collection = "memory",
                                      .dimension = 3,
                                      .vector = {0.1f, 0.2f, 0.3f}};
   assert(aimee_db3_apply_encode(&fixture_apply, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_apply) && memcmp(wire, expected_apply, length) == 0);
}

int main(void)
{
   test_default_external_and_authorization();
   test_fail_closed_and_explicit_fallback();
   test_invalid_requests();
   test_wire_codecs();
   puts("test_db3_route: routing, fallback, revalidation, and codecs passed");
   return 0;
}
