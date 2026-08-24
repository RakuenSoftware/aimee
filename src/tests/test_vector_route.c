#include <aimee/db2/vector_route.h>

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

_Static_assert(AIMEE_VECTOR_EVENT_CAPABILITIES == 0x80030001u, "DB3 capability event drift");
_Static_assert(AIMEE_VECTOR_EVENT_APPLY == 0x80030002u, "DB3 apply event drift");
_Static_assert(AIMEE_VECTOR_EVENT_APPLIED == 0x80030003u, "DB3 applied event drift");
_Static_assert(AIMEE_VECTOR_EVENT_SEARCH == 0x80030004u, "DB3 search event drift");
_Static_assert(AIMEE_VECTOR_EVENT_ROUTE == 0x80030005u, "DB3 route event drift");

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

static int search(void *context, const aimee_vector_search_request_t *request,
                  aimee_vector_search_reply_t *reply)
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

/* The caller owns the vector now, so the fixture is handed the buffer it should
 * point at rather than carrying one of its own. Every real caller makes the same
 * change; the tests should not be exempt from it. */
static aimee_vector_search_request_t request(float *vector)
{
   aimee_vector_search_request_t value = {0};
   value.request_id = 91;
   value.required_generation = 7;
   strcpy(value.workspace, "workspace-a");
   strcpy(value.project, "project-a");
   strcpy(value.record_type, "memory");
   value.dimension = 3;
   value.top_k = 3;
   vector[0] = 0.25f;
   vector[1] = -0.5f;
   vector[2] = 0.75f;
   value.vector = vector;
   return value;
}

static void test_default_external_and_authorization(void)
{
   search_state_t internal = {.first_id = 10}, external = {.first_id = 20};
   auth_state_t auth = {.reject_id = 11};
   aimee_vector_route_t route;
   assert(aimee_vector_route_init(&route, search, &internal, authorize, &auth) == 0);
   float vec[3];
   aimee_vector_search_request_t req = request(vec);
   aimee_vector_search_outcome_t out;

   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_OK);
   assert(out.route == AIMEE_VECTOR_ROUTE_DEFAULT_PGVECTOR && out.selected_principal == 0);
   assert(internal.calls == 1 && external.calls == 0 && out.reply.count == 2);
   assert(out.reply.candidates[0].point_id == 10 && out.reply.candidates[1].point_id == 12);

   auth.reject_id = 21;
   assert(aimee_vector_route_select(&route, 1001, 1, 0, search, &external) == 0);
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_OK);
   assert(out.route == AIMEE_VECTOR_ROUTE_EXTERNAL && out.selected_principal == 1001);
   assert(internal.calls == 1 && external.calls == 1 && out.reply.count == 2);
   assert(out.reply.candidates[0].point_id == 20 && out.reply.candidates[1].point_id == 22);

   aimee_vector_route_clear(&route);
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_OK);
   assert(out.route == AIMEE_VECTOR_ROUTE_DEFAULT_PGVECTOR && internal.calls == 2);
}

static void test_fail_closed_and_explicit_fallback(void)
{
   search_state_t internal = {.first_id = 10}, external = {.first_id = 20, .mode = 1};
   auth_state_t auth = {.reject_id = -1};
   aimee_vector_route_t route;
   float vec[3];
   aimee_vector_search_request_t req = request(vec);
   aimee_vector_search_outcome_t out;
   assert(aimee_vector_route_init(&route, search, &internal, authorize, &auth) == 0);

   assert(aimee_vector_route_select(&route, 1001, 0, 0, search, &external) == 0);
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_UNAVAILABLE);
   assert(out.route == AIMEE_VECTOR_ROUTE_EXTERNAL &&
          out.external_error == AIMEE_VECTOR_UNAVAILABLE);
   assert(internal.calls == 0 && external.calls == 0);

   assert(aimee_vector_route_select(&route, 1001, 1, 0, search, &external) == 0);
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
          AIMEE_VECTOR_PROVIDER_FAILURE);
   assert(internal.calls == 0 && external.calls == 1);

   assert(aimee_vector_route_select(&route, 1001, 1, 1, search, &external) == 0);
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_OK);
   assert(out.route == AIMEE_VECTOR_ROUTE_EXPLICIT_FALLBACK);
   assert(out.external_error == AIMEE_VECTOR_PROVIDER_FAILURE);
   assert(internal.calls == 1 && external.calls == 2);

   external.mode = 3;
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_OK);
   assert(out.route == AIMEE_VECTOR_ROUTE_EXPLICIT_FALLBACK);
   assert(out.external_error == AIMEE_VECTOR_INVALID_RESPONSE);
   assert(internal.calls == 2 && external.calls == 3);

   route.fallback_enabled = 0;
   for (int mode = 2; mode <= 5; ++mode)
   {
      external.mode = mode;
      assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
             AIMEE_VECTOR_INVALID_RESPONSE);
      assert(out.reply.count == 3 || mode == 2);
   }

   external.mode = 0;
   auth.fail = 1;
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) == AIMEE_VECTOR_INTERNAL);
}

static void test_invalid_requests(void)
{
   search_state_t internal = {.first_id = 1};
   auth_state_t auth = {0};
   aimee_vector_route_t route;
   aimee_vector_search_outcome_t out;
   assert(aimee_vector_route_init(&route, search, &internal, authorize, &auth) == 0);
   float vec[3];
   aimee_vector_search_request_t req = request(vec);

   vec[1] = INFINITY;
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
          AIMEE_VECTOR_INVALID_REQUEST);
   req = request(vec);
   req.top_k = 0;
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
          AIMEE_VECTOR_INVALID_REQUEST);
   req = request(vec);
   req.workspace[0] = '\0';
   req.project[0] = '\0';
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
          AIMEE_VECTOR_INVALID_REQUEST);
   req = request(vec);
   memset(req.record_type, 'x', sizeof(req.record_type));
   assert(aimee_vector_memory_candidates_search(&route, &req, &out) ==
          AIMEE_VECTOR_INVALID_REQUEST);
   assert(internal.calls == 0);
}

static void test_wire_codecs(void)
{
   uint8_t wire[512], mutated[512];
   size_t length = 0;
   float vec[3], decoded_vec[AIMEE_VECTOR_MAX_DIM];
   aimee_vector_filter_view_t decoded_filters;
   aimee_vector_search_request_t req = request(vec), decoded_req;
   assert(aimee_vector_search_request_encode(&req, wire, sizeof(wire), &length) == 0);
   assert(length == 36 + strlen(req.workspace) + strlen(req.project) + strlen(req.record_type) +
                        req.dimension * sizeof(float));
   assert(aimee_vector_search_request_decode(wire, length, &decoded_req, decoded_vec,
                                             AIMEE_VECTOR_MAX_DIM, &decoded_filters) == 0);
   assert(decoded_req.request_id == req.request_id && decoded_req.dimension == req.dimension);
   assert(memcmp(decoded_req.vector, req.vector, req.dimension * sizeof(float)) == 0);
   assert(aimee_vector_search_request_decode(wire, length - 1, &decoded_req, decoded_vec,
                                             AIMEE_VECTOR_MAX_DIM, &decoded_filters) != 0);
   memcpy(mutated, wire, length);
   mutated[34] = 1;
   assert(aimee_vector_search_request_decode(mutated, length, &decoded_req, decoded_vec,
                                             AIMEE_VECTOR_MAX_DIM, &decoded_filters) != 0);
   memcpy(mutated, wire, length);
   mutated[length - 1] = 0x7f;
   mutated[length - 2] = 0x80;
   mutated[length - 3] = 0;
   mutated[length - 4] = 0;
   assert(aimee_vector_search_request_decode(mutated, length, &decoded_req, decoded_vec,
                                             AIMEE_VECTOR_MAX_DIM, &decoded_filters) != 0);

   aimee_vector_search_reply_t reply = {.request_id = req.request_id,
                                        .generation = req.required_generation,
                                        .count = 2,
                                        .candidates = {{101, 0.9}, {102, 0.8}}};
   aimee_vector_search_reply_t decoded_reply;
   assert(aimee_vector_search_reply_encode(&reply, wire, sizeof(wire), &length) == 0);
   assert(aimee_vector_search_reply_decode(wire, length, &decoded_reply) == 0);
   assert(aimee_vector_search_reply_validate(&req, &decoded_reply) == 0);
   assert(aimee_vector_search_reply_decode(wire, length - 1, &decoded_reply) != 0);
   assert(aimee_vector_search_reply_decode(wire, length, &decoded_reply) == 0);
   decoded_reply.candidates[1].point_id = 101;
   assert(aimee_vector_search_reply_validate(&req, &decoded_reply) != 0);
   assert(aimee_vector_search_reply_encode(&decoded_reply, wire, sizeof(wire), &length) != 0);

   float apply_vec[3] = {0.1f, 0.2f, 0.3f};
   aimee_vector_apply_t apply = {.operation_id = 1001,
                                 .generation = 7,
                                 .point_id = 101,
                                 .kind = AIMEE_VECTOR_APPLY_UPSERT,
                                 .collection = "memory",
                                 .dimension = 3,
                                 .vector = apply_vec};
   aimee_vector_apply_t decoded_apply;
   assert(aimee_vector_apply_encode(&apply, wire, sizeof(wire), &length) == 0);
   assert(aimee_vector_apply_decode(wire, length, &decoded_apply, decoded_vec,
                                    AIMEE_VECTOR_MAX_DIM) == 0);
   assert(decoded_apply.operation_id == apply.operation_id && decoded_apply.dimension == 3);
   assert(aimee_vector_apply_decode(wire, length - 1, &decoded_apply, decoded_vec,
                                    AIMEE_VECTOR_MAX_DIM) != 0);
   apply_vec[0] = NAN;
   assert(aimee_vector_apply_encode(&apply, wire, sizeof(wire), &length) != 0);
   apply_vec[0] = 0.1f;
   apply.kind = AIMEE_VECTOR_APPLY_DELETE;
   assert(aimee_vector_apply_validate(&apply) != 0);
   apply.dimension = 0;
   assert(aimee_vector_apply_validate(&apply) == 0);

   apply.kind = AIMEE_VECTOR_APPLY_UPSERT;
   apply.dimension = 3;
   apply.label_count = 3;
   strcpy(apply.labels[0].key, "project");
   strcpy(apply.labels[0].value, "project with space");
   strcpy(apply.labels[1].key, "record_type");
   strcpy(apply.labels[1].value, "memory");
   strcpy(apply.labels[2].key, "workspace");
   strcpy(apply.labels[2].value, "workspace-a");
   assert(aimee_vector_apply_encode(&apply, wire, sizeof(wire), &length) == 0);
   assert(wire[4] == AIMEE_VECTOR_APPLY_V2_VERSION && wire[5] == 0);
   assert(aimee_vector_apply_decode(wire, length, &decoded_apply, decoded_vec,
                                    AIMEE_VECTOR_MAX_DIM) == 0);
   assert(decoded_apply.label_count == 3);
   assert(strcmp(decoded_apply.labels[0].value, "project with space") == 0);
   assert(strcmp(decoded_apply.labels[2].key, "workspace") == 0);
   memcpy(mutated, wire, length);
   mutated[36] = 0;
   mutated[37] = 0;
   assert(aimee_vector_apply_decode(mutated, length, &decoded_apply, decoded_vec,
                                    AIMEE_VECTOR_MAX_DIM) != 0);
   memcpy(mutated, wire, length);
   mutated[38]++;
   assert(aimee_vector_apply_decode(mutated, length, &decoded_apply, decoded_vec,
                                    AIMEE_VECTOR_MAX_DIM) != 0);
   strcpy(apply.labels[1].key, "project");
   assert(aimee_vector_apply_validate(&apply) != 0);
   strcpy(apply.labels[1].key, "record_type");
   apply.labels[1].value[0] = '\n';
   apply.labels[1].value[1] = '\0';
   assert(aimee_vector_apply_validate(&apply) != 0);
   strcpy(apply.labels[1].value, "memory");
   apply.kind = AIMEE_VECTOR_APPLY_DELETE;
   apply.dimension = 0;
   assert(aimee_vector_apply_validate(&apply) != 0);

   /* Generated tests/baselines/modules/vector-wire-v1.json replays these exact
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
   const uint8_t expected_apply_v2[] = {
       0x44, 0x42, 0x33, 0x41, 0x02, 0x00, 0x01, 0x00, 0xea, 0x03, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2a, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x06, 0x00, 0x03, 0x00, 0x03, 0x00, 0x41, 0x00, 0x6d, 0x65,
       0x6d, 0x6f, 0x72, 0x79, 0x9a, 0x99, 0x99, 0x3e, 0xcd, 0xcc, 0x4c, 0x3e, 0xcd, 0xcc,
       0xcc, 0x3d, 0x07, 0x00, 0x09, 0x00, 0x70, 0x72, 0x6f, 0x6a, 0x65, 0x63, 0x74, 0x70,
       0x72, 0x6f, 0x6a, 0x65, 0x63, 0x74, 0x2d, 0x61, 0x0b, 0x00, 0x06, 0x00, 0x72, 0x65,
       0x63, 0x6f, 0x72, 0x64, 0x5f, 0x74, 0x79, 0x70, 0x65, 0x6d, 0x65, 0x6d, 0x6f, 0x72,
       0x79, 0x09, 0x00, 0x0b, 0x00, 0x77, 0x6f, 0x72, 0x6b, 0x73, 0x70, 0x61, 0x63, 0x65,
       0x77, 0x6f, 0x72, 0x6b, 0x73, 0x70, 0x61, 0x63, 0x65, 0x2d, 0x61};
   float fixture_request_vec[3] = {0.3f, 0.2f, 0.1f};
   aimee_vector_search_request_t fixture_request = {.request_id = 77,
                                                    .required_generation = 7,
                                                    .workspace = "workspace-a",
                                                    .project = "project-a",
                                                    .record_type = "memory",
                                                    .dimension = 3,
                                                    .top_k = 2,
                                                    .vector = fixture_request_vec};
   assert(aimee_vector_search_request_encode(&fixture_request, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_request) && memcmp(wire, expected_request, length) == 0);
   aimee_vector_search_reply_t fixture_reply = {
       .request_id = 77, .generation = 7, .count = 2, .candidates = {{41, 0.95}, {42, 0.75}}};
   assert(aimee_vector_search_reply_encode(&fixture_reply, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_reply) && memcmp(wire, expected_reply, length) == 0);
   float fixture_apply_vec[3] = {0.1f, 0.2f, 0.3f};
   aimee_vector_apply_t fixture_apply = {.operation_id = 1001,
                                         .generation = 7,
                                         .point_id = 41,
                                         .kind = AIMEE_VECTOR_APPLY_UPSERT,
                                         .collection = "memory",
                                         .dimension = 3,
                                         .vector = fixture_apply_vec};
   assert(aimee_vector_apply_encode(&fixture_apply, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_apply) && memcmp(wire, expected_apply, length) == 0);
   float fixture_apply_v2_vec[3] = {0.3f, 0.2f, 0.1f};
   aimee_vector_apply_t fixture_apply_v2 = {.operation_id = 1002,
                                            .generation = 7,
                                            .point_id = 42,
                                            .kind = AIMEE_VECTOR_APPLY_UPSERT,
                                            .collection = "memory",
                                            .dimension = 3,
                                            .vector = fixture_apply_v2_vec,
                                            .label_count = 3,
                                            .labels = {
                                                {.key = "project", .value = "project-a"},
                                                {.key = "record_type", .value = "memory"},
                                                {.key = "workspace", .value = "workspace-a"},
                                            }};
   assert(aimee_vector_apply_encode(&fixture_apply_v2, wire, sizeof(wire), &length) == 0);
   assert(length == sizeof(expected_apply_v2) && memcmp(wire, expected_apply_v2, length) == 0);
}

static void test_every_filter_a_search_has_is_carried(void)
{
   /* This test used to assert the opposite: that four filters and two
    * predicates were REFUSED. That was the right assertion about a wire which
    * could not carry them, and it would now pin the defect.
    *
    * The six things every DB2 vector search filters by, and where each goes:
    *
    *   labels             one EQ each -- four curator searches
    *   kinds              one IN      -- two memory searches
    *   exclude_project    one NE      -- two scoped kb searches
    *   rank_column        the collection field -- claim and code-unit search
    *   visibility         one IN over a multi-valued label -- scope membership
    *   current_generation one EQ      -- currency
    */
   float vec[3] = {0.25f, -0.5f, 0.75f};
   const char *kinds[] = {"note", "fact"};
   const char *label_keys[] = {"status"};
   const char *label_values[] = {"open"};
   const char *visibility[] = {"proj:project-a", "ws:workspace-a", "global", "untagged"};
   aimee_vector_filter_t predicates[AIMEE_VECTOR_MAX_FILTERS];
   aimee_vector_search_request_t request;

   aimee_vector_search_filters_t everything = {.workspace = "workspace-a",
                                               .project = "project-a",
                                               .record_type = "memory",
                                               .rank_column = "subj_attr",
                                               .exclude_project = "project-b",
                                               .kinds = kinds,
                                               .kind_count = 2,
                                               .label_keys = label_keys,
                                               .label_values = label_values,
                                               .label_count = 1,
                                               .visibility = visibility,
                                               .visibility_count = 4,
                                               .current_generation = "7"};
   assert(aimee_vector_search_filters_expressible(&everything) == 1);
   assert(aimee_vector_search_request_build(&everything, 91, 7, vec, 3, 3, predicates,
                                            AIMEE_VECTOR_MAX_FILTERS, &request) == 0);
   /* Five predicates: one label, kinds, exclude_project, visibility, generation.
    * rank_column is the collection field rather than a predicate. */
   assert(request.filter_count == 5);
   assert(strcmp(request.collection, "subj_attr") == 0);

   /* And they survive the wire, which is the half a struct comparison cannot
    * show. Walked with the cursor a provider would use. */
   uint8_t wire[4096];
   size_t length = 0;
   assert(aimee_vector_search_request_encode(&request, wire, sizeof(wire), &length) == 0);
   aimee_vector_search_request_t decoded;
   float decoded_vector[AIMEE_VECTOR_MAX_DIM];
   aimee_vector_filter_view_t view;
   assert(aimee_vector_search_request_decode(wire, length, &decoded, decoded_vector,
                                             AIMEE_VECTOR_MAX_DIM, &view) == 0);
   assert(strcmp(decoded.collection, "subj_attr") == 0);
   assert(decoded.filter_count == 5);

   int saw_kinds = 0, saw_exclude = 0, saw_visibility = 0, saw_generation = 0, saw_label = 0;
   aimee_vector_filter_entry_t entry;
   while (aimee_vector_filter_next(&view, &entry) == 1)
   {
      const char *value = NULL;
      size_t value_length = 0;
      if (entry.key_length == 4 && memcmp(entry.key, "kind", 4) == 0)
      {
         assert(entry.op == AIMEE_VECTOR_FILTER_IN && entry.value_count == 2);
         assert(aimee_vector_filter_value(&view, &entry, 1, &value, &value_length) == 0);
         assert(value_length == 4 && memcmp(value, "fact", 4) == 0);
         saw_kinds = 1;
      }
      else if (entry.key_length == 7 && memcmp(entry.key, "project", 7) == 0)
      {
         assert(entry.op == AIMEE_VECTOR_FILTER_NE && entry.value_count == 1);
         saw_exclude = 1;
      }
      else if (entry.key_length == 10 && memcmp(entry.key, "visibility", 10) == 0)
      {
         /* The four-way scope disjunction, as ONE set-membership predicate. */
         assert(entry.op == AIMEE_VECTOR_FILTER_IN && entry.value_count == 4);
         assert(aimee_vector_filter_value(&view, &entry, 3, &value, &value_length) == 0);
         assert(value_length == 8 && memcmp(value, "untagged", 8) == 0);
         saw_visibility = 1;
      }
      else if (entry.key_length == 10 && memcmp(entry.key, "generation", 10) == 0)
      {
         assert(entry.op == AIMEE_VECTOR_FILTER_EQ && entry.value_count == 1);
         assert(aimee_vector_filter_value(&view, &entry, 0, &value, &value_length) == 0);
         assert(value_length == 1 && value[0] == '7');
         saw_generation = 1;
      }
      else if (entry.key_length == 6 && memcmp(entry.key, "status", 6) == 0)
      {
         assert(entry.op == AIMEE_VECTOR_FILTER_EQ);
         saw_label = 1;
      }
   }
   assert(saw_kinds && saw_exclude && saw_visibility && saw_generation && saw_label);
}

static void test_a_request_with_nothing_extra_is_still_version_one(void)
{
   /* Byte for byte, so a provider that speaks only version 1 keeps receiving
    * every request it could already serve. The compatibility is a consequence
    * of the encoding rather than a promise made about it. */
   float vec[3];
   aimee_vector_filter_t predicates[AIMEE_VECTOR_MAX_FILTERS];
   aimee_vector_search_request_t plain;
   aimee_vector_search_filters_t bare = {
       .workspace = "workspace-a", .project = "project-a", .record_type = "memory"};
   vec[0] = 0.25f;
   vec[1] = -0.5f;
   vec[2] = 0.75f;
   assert(aimee_vector_search_request_build(&bare, 91, 7, vec, 3, 3, predicates,
                                            AIMEE_VECTOR_MAX_FILTERS, &plain) == 0);
   assert(plain.filter_count == 0 && plain.collection[0] == '\0');

   uint8_t wire[512];
   size_t length = 0;
   assert(aimee_vector_search_request_encode(&plain, wire, sizeof(wire), &length) == 0);
   assert(length == AIMEE_VECTOR_SEARCH_REQUEST_HEADER + strlen("workspace-a") +
                        strlen("project-a") + strlen("memory") + 3 * sizeof(float));
   uint16_t version = (uint16_t)(wire[4] | (wire[5] << 8));
   assert(version == AIMEE_VECTOR_WIRE_VERSION);
}

static void test_one_meaning_has_one_encoding(void)
{
   /* The encoder picks version 1 when there is nothing version 2 carries, so a
    * version 2 frame with neither a collection nor a filter is one nobody
    * writes. Accepting it would make two byte sequences mean one request, and a
    * provider that treats the versions differently would answer differently for
    * the same question. */
   float vec[3] = {0.25f, -0.5f, 0.75f};
   aimee_vector_filter_t predicates[AIMEE_VECTOR_MAX_FILTERS];
   aimee_vector_search_request_t request;
   aimee_vector_search_filters_t bare = {
       .workspace = "workspace-a", .project = "project-a", .record_type = "memory"};
   assert(aimee_vector_search_request_build(&bare, 91, 7, vec, 3, 3, predicates,
                                            AIMEE_VECTOR_MAX_FILTERS, &request) == 0);

   uint8_t wire[512];
   size_t length = 0;
   assert(aimee_vector_search_request_encode(&request, wire, sizeof(wire), &length) == 0);

   /* Hand-build the version 2 form of the same request: the v1 header widened,
    * the version bumped, the three new fields zero. Every byte after the header
    * is identical, which is what makes it the same request. */
   uint8_t forged[512];
   size_t header = AIMEE_VECTOR_SEARCH_REQUEST_HEADER;
   size_t v2_header = AIMEE_VECTOR_SEARCH_REQUEST_V2_HEADER;
   memset(forged, 0, v2_header);
   memcpy(forged, wire, header);
   forged[4] = (uint8_t)AIMEE_VECTOR_SEARCH_REQUEST_V2_VERSION;
   forged[5] = 0;
   forged[6] = (uint8_t)v2_header;
   forged[7] = 0;
   memcpy(forged + v2_header, wire + header, length - header);
   size_t forged_length = length - header + v2_header;

   aimee_vector_search_request_t decoded;
   float decoded_vector[AIMEE_VECTOR_MAX_DIM];
   aimee_vector_filter_view_t view;
   assert(aimee_vector_search_request_decode(forged, forged_length, &decoded, decoded_vector,
                                             AIMEE_VECTOR_MAX_DIM, &view) != 0);

   /* And the form the encoder does write still decodes. */
   assert(aimee_vector_search_request_decode(wire, length, &decoded, decoded_vector,
                                             AIMEE_VECTOR_MAX_DIM, &view) == 0);
}

static void test_what_does_not_fit_is_still_refused(void)
{
   /* The refusals that remain are arithmetic rather than vocabulary: a set over
    * the ceiling, more predicates than there are fields for, half a label pair.
    * Still refusals rather than trims -- a filter set silently narrowed is a
    * search with a predicate missing, which returns more rows than it should
    * and looks exactly like a correct answer. */
   float vec[3] = {0.25f, -0.5f, 0.75f};
   aimee_vector_filter_t predicates[AIMEE_VECTOR_MAX_FILTERS];
   aimee_vector_search_request_t request;
   const char *many[AIMEE_VECTOR_MAX_FILTER_VALUES + 1];
   for (size_t i = 0; i < sizeof(many) / sizeof(many[0]); ++i)
      many[i] = "value";

   aimee_vector_search_filters_t base = {
       .workspace = "workspace-a", .project = "project-a", .record_type = "memory"};

   aimee_vector_search_filters_t too_many_values = base;
   too_many_values.kinds = many;
   too_many_values.kind_count = AIMEE_VECTOR_MAX_FILTER_VALUES + 1;
   assert(aimee_vector_search_filters_expressible(&too_many_values) == 0);

   /* A label pair with one half missing. Guessing the other half is how a
    * search asks a question nobody wrote. */
   const char *keys[] = {"status"};
   aimee_vector_search_filters_t half_a_pair = base;
   half_a_pair.label_keys = keys;
   half_a_pair.label_count = 1;
   assert(aimee_vector_search_filters_expressible(&half_a_pair) == 0);

   /* More predicates than the caller gave room for. */
   const char *label_keys[AIMEE_VECTOR_MAX_FILTERS];
   const char *label_values[AIMEE_VECTOR_MAX_FILTERS];
   for (size_t i = 0; i < AIMEE_VECTOR_MAX_FILTERS; ++i)
   {
      label_keys[i] = "status";
      label_values[i] = "open";
   }
   aimee_vector_search_filters_t full = base;
   full.label_keys = label_keys;
   full.label_values = label_values;
   full.label_count = AIMEE_VECTOR_MAX_FILTERS;
   assert(aimee_vector_search_filters_expressible(&full) == 1);
   assert(aimee_vector_search_request_build(&full, 91, 7, vec, 3, 3, predicates, 4, &request) != 0);

   /* One more than there are filter slots. */
   aimee_vector_search_filters_t over = full;
   over.current_generation = "7";
   assert(aimee_vector_search_filters_expressible(&over) == 0);
}

int main(void)
{
   test_default_external_and_authorization();
   test_fail_closed_and_explicit_fallback();
   test_invalid_requests();
   test_wire_codecs();
   test_every_filter_a_search_has_is_carried();
   test_a_request_with_nothing_extra_is_still_version_one();
   test_what_does_not_fit_is_still_refused();
   test_one_meaning_has_one_encoding();
   puts("test_vector_route: routing, fallback, revalidation, and codecs passed");
   return 0;
}
