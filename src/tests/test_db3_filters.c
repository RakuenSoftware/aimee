/* test_db3_filters.c: the v2 search request's filter list, in C.
 *
 * The frozen vector below is the same one server-go/db3 asserts against. Both
 * implementations encoding these exact bytes is what makes them interoperable;
 * a divergence here means a C sender and a Go provider would disagree about
 * what a request said while both believing they were correct.
 */
#include <aimee/db2/db3_route.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* tests/baselines/modules/db3-wire-v1.json, search_request_v2_hex. */
static const char *const FROZEN_V2_HEX =
    "44423353020028004d0000000000000007000000000000000b00090006000300"
    "0200000002002900776f726b73706163652d6170726f6a6563742d616d656d6f"
    "72799a99993ecdcc4c3ecdcccc3d0700090070726f6a65637470726f6a656374"
    "2d610b0006007265636f72645f747970656d656d6f7279";

static size_t unhex(const char *hex, uint8_t *out, size_t capacity)
{
   size_t n = 0;
   int high = -1;
   for (const char *p = hex; *p; ++p)
   {
      int value;
      if (*p == ' ')
         continue;
      if (*p >= '0' && *p <= '9')
         value = *p - '0';
      else if (*p >= 'a' && *p <= 'f')
         value = 10 + (*p - 'a');
      else
         return 0;
      if (high < 0)
         high = value;
      else
      {
         if (n >= capacity)
            return 0;
         out[n++] = (uint8_t)((high << 4) | value);
         high = -1;
      }
   }
   return high < 0 ? n : 0;
}

static void fill_request(aimee_db3_search_request_t *request)
{
   memset(request, 0, sizeof(*request));
   request->request_id = 77;
   request->required_generation = 7;
   snprintf(request->workspace, sizeof(request->workspace), "workspace-a");
   snprintf(request->project, sizeof(request->project), "project-a");
   snprintf(request->record_type, sizeof(request->record_type), "memory");
   request->dimension = 3;
   request->top_k = 2;
   request->vector[0] = .3f;
   request->vector[1] = .2f;
   request->vector[2] = .1f;
}

static void add_filters(aimee_db3_search_request_t *request)
{
   request->filter_count = 2;
   snprintf(request->filters[0].key, sizeof(request->filters[0].key), "project");
   snprintf(request->filters[0].value, sizeof(request->filters[0].value), "project-a");
   snprintf(request->filters[1].key, sizeof(request->filters[1].key), "record_type");
   snprintf(request->filters[1].value, sizeof(request->filters[1].value), "memory");
}

int main(void)
{
   uint8_t buffer[4096];
   size_t length = 0;

   /* A request with no filters must encode as v1: same version, same header. */
   aimee_db3_search_request_t plain;
   fill_request(&plain);
   assert(aimee_db3_search_request_encode(&plain, buffer, sizeof(buffer), &length) == 0);
   assert(buffer[4] == AIMEE_DB3_WIRE_VERSION && buffer[5] == 0);
   assert(buffer[6] == AIMEE_DB3_SEARCH_REQUEST_HEADER && buffer[7] == 0);
   size_t v1_length = length;

   /* Adding filters moves it to v2 and grows the header. */
   aimee_db3_search_request_t filtered;
   fill_request(&filtered);
   add_filters(&filtered);
   assert(aimee_db3_search_request_encode(&filtered, buffer, sizeof(buffer), &length) == 0);
   assert(buffer[4] == AIMEE_DB3_SEARCH_REQUEST_V2_VERSION && buffer[5] == 0);
   assert(buffer[6] == AIMEE_DB3_SEARCH_REQUEST_V2_HEADER && buffer[7] == 0);
   assert(length > v1_length);

   /* And it must be byte-for-byte what Go emits. */
   uint8_t frozen[4096];
   size_t frozen_length = unhex(FROZEN_V2_HEX, frozen, sizeof(frozen));
   assert(frozen_length != 0);
   assert(frozen_length == length);
   assert(memcmp(buffer, frozen, length) == 0);

   /* The frozen bytes must decode back to the same request. */
   aimee_db3_search_request_t decoded;
   assert(aimee_db3_search_request_decode(frozen, frozen_length, &decoded) == 0);
   assert(decoded.filter_count == 2);
   assert(strcmp(decoded.filters[0].key, "project") == 0);
   assert(strcmp(decoded.filters[0].value, "project-a") == 0);
   assert(strcmp(decoded.filters[1].key, "record_type") == 0);
   assert(strcmp(decoded.filters[1].value, "memory") == 0);
   assert(decoded.top_k == 2 && decoded.dimension == 3);

   /* Unsorted or duplicated keys give one filter set two encodings, so both are
    * refused rather than normalised. */
   aimee_db3_search_request_t unsorted;
   fill_request(&unsorted);
   unsorted.filter_count = 2;
   snprintf(unsorted.filters[0].key, sizeof(unsorted.filters[0].key), "record_type");
   snprintf(unsorted.filters[0].value, sizeof(unsorted.filters[0].value), "memory");
   snprintf(unsorted.filters[1].key, sizeof(unsorted.filters[1].key), "project");
   snprintf(unsorted.filters[1].value, sizeof(unsorted.filters[1].value), "project-a");
   assert(aimee_db3_search_request_validate(&unsorted) != 0);
   assert(aimee_db3_search_request_encode(&unsorted, buffer, sizeof(buffer), &length) != 0);

   aimee_db3_search_request_t duplicated;
   fill_request(&duplicated);
   duplicated.filter_count = 2;
   snprintf(duplicated.filters[0].key, sizeof(duplicated.filters[0].key), "project");
   snprintf(duplicated.filters[0].value, sizeof(duplicated.filters[0].value), "a");
   snprintf(duplicated.filters[1].key, sizeof(duplicated.filters[1].key), "project");
   snprintf(duplicated.filters[1].value, sizeof(duplicated.filters[1].value), "b");
   assert(aimee_db3_search_request_validate(&duplicated) != 0);

   /* More filters than the contract allows. */
   aimee_db3_search_request_t too_many;
   fill_request(&too_many);
   too_many.filter_count = AIMEE_DB3_MAX_LABELS + 1;
   assert(aimee_db3_search_request_validate(&too_many) != 0);

   /* A v2 frame carrying no filters is refused. */
   assert(aimee_db3_search_request_encode(&filtered, buffer, sizeof(buffer), &length) == 0);
   uint8_t empty_v2[4096];
   size_t filters_bytes = (size_t)buffer[38] | ((size_t)buffer[39] << 8);
   memcpy(empty_v2, buffer, length - filters_bytes);
   empty_v2[36] = empty_v2[37] = empty_v2[38] = empty_v2[39] = 0;
   assert(aimee_db3_search_request_decode(empty_v2, length - filters_bytes, &decoded) != 0);

   /* A version that disagrees with the header length is refused. */
   uint8_t mismatched[4096];
   memcpy(mismatched, buffer, length);
   mismatched[4] = AIMEE_DB3_WIRE_VERSION; /* claim v1, keep the v2 header */
   assert(aimee_db3_search_request_decode(mismatched, length, &decoded) != 0);

   memcpy(mismatched, buffer, length);
   mismatched[4] = 99;
   assert(aimee_db3_search_request_decode(mismatched, length, &decoded) != 0);

   /* A declared filter length that does not match what the filters consume
    * would let a frame hide trailing bytes. */
   uint8_t tampered[4096];
   memcpy(tampered, buffer, length);
   tampered[38] = (uint8_t)(tampered[38] - 1);
   assert(aimee_db3_search_request_decode(tampered, length, &decoded) != 0);

   /* A v1 frame still decodes, and reports no filters. */
   assert(aimee_db3_search_request_encode(&plain, buffer, sizeof(buffer), &length) == 0);
   assert(aimee_db3_search_request_decode(buffer, length, &decoded) == 0);
   assert(decoded.filter_count == 0);

   printf("test_db3_filters: v2 filters encode, decode, and match the frozen vector\n");
   return 0;
}
