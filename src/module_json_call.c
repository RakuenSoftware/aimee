/* module_json_call.c: see headers/module_json_call.h.
 *
 * Deliberately does not decide what a failure means. Whether an unreachable
 * module is a 502, an API error, or a silent no-op is the caller's policy, and
 * the three existing consumers answer it three different ways.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "headers/module_json_call.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

uint64_t aimee_module_call_deadline_ns(int timeout_ms)
{
   if (timeout_ms <= 0)
      return 0;
   struct timespec ts;
   if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
      return 0; /* no clock: fall back to "no deadline" rather than a past one,
                 * which would fail every call instantly. */
   return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec +
          (uint64_t)timeout_ms * 1000000ull;
}

cJSON *aimee_module_json_call_raw(uint32_t event_kind, uint32_t stage_id, const char *body,
                                  size_t body_len, size_t max_body, int timeout_ms,
                                  aimee_module_call_result_t *result)
{
   if (result)
      *result = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   if (!body || max_body == 0 || max_body > UINT32_MAX)
      return NULL;

   if (body_len > max_body)
   {
      if (result)
         *result = AIMEE_MODULE_CALL_INVALID_REQUEST;
      return NULL;
   }

   /* A module that is not attached is the ordinary case for an optional one. */
   if (!obs_bus_module_available(event_kind))
   {
      if (result)
         *result = AIMEE_MODULE_CALL_CAPABILITY_ABSENT;
      return NULL;
   }

   char *response = malloc(max_body);
   if (!response)
   {
      if (result)
         *result = AIMEE_MODULE_CALL_INTERNAL;
      return NULL;
   }

   uint32_t response_len = 0;
   aimee_module_call_result_t rc = obs_bus_module_call(
       event_kind, stage_id, 0, aimee_module_call_deadline_ns(timeout_ms), body, (uint32_t)body_len,
       response, (uint32_t)max_body, &response_len, NULL, NULL);
   if (result)
      *result = rc;
   if (rc != AIMEE_MODULE_CALL_OK)
   {
      free(response);
      return NULL;
   }

   cJSON *parsed = cJSON_ParseWithLength(response, response_len);
   free(response);
   /* rc stays OK on an unparseable body: the module answered, the payload was
    * the problem. The caller distinguishes the two by result vs NULL return. */
   return parsed;
}

cJSON *aimee_module_json_call(uint32_t event_kind, uint32_t stage_id, cJSON *request,
                              size_t max_body, int timeout_ms, aimee_module_call_result_t *result)
{
   if (result)
      *result = AIMEE_MODULE_CALL_INVALID_ARGUMENT;
   if (!request)
      return NULL;

   /* Ask before serializing. Printing a body for a module that is not attached
    * is pure waste, and this is the ordinary case for an optional module. */
   if (!obs_bus_module_available(event_kind))
   {
      cJSON_Delete(request);
      if (result)
         *result = AIMEE_MODULE_CALL_CAPABILITY_ABSENT;
      return NULL;
   }

   char *wire = cJSON_PrintUnformatted(request);
   cJSON_Delete(request);
   if (!wire)
   {
      if (result)
         *result = AIMEE_MODULE_CALL_INTERNAL;
      return NULL;
   }

   cJSON *parsed = aimee_module_json_call_raw(event_kind, stage_id, wire, strlen(wire), max_body,
                                              timeout_ms, result);
   free(wire);
   return parsed;
}
