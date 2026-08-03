#include <aimee/core/event_bus/module_runtime.h>
#include <aimee/response-composition/module_api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t fnv1a(const uint8_t *value, size_t len)
{
   uint64_t hash = 1469598103934665603ULL;
   for (size_t i = 0; i < len; ++i)
   {
      hash ^= value[i];
      hash *= 1099511628211ULL;
   }
   return hash;
}

static uint64_t fnv_field(uint64_t hash, uint64_t seed, const uint8_t *value, size_t len)
{
   hash ^= seed;
   for (size_t i = 0; i < len; ++i)
   {
      hash ^= value[i];
      hash *= 1099511628211ULL;
   }
   hash ^= 0x1e;
   return hash * 1099511628211ULL;
}

aimee_module_status_t aimee_module_handler(
    const aimee_module_invocation_t *invocation, const uint8_t *request_body,
    uint32_t request_len, uint8_t *response_body, uint32_t response_capacity,
    uint32_t *response_len, void *user_data)
{
   (void)user_data;
   if (!invocation || !response_len || invocation->stage_id != AIMEE_RESPONSE_STAGE_COMPOSE ||
       !request_body || request_len < AIMEE_RESPONSE_REQUEST_HEADER_LEN ||
       response_capacity < 5 ||
       aimee_response_get_u32(request_body) != AIMEE_RESPONSE_REQUEST_MAGIC ||
       request_body[4] != AIMEE_RESPONSE_WIRE_VERSION || request_body[5] != 0 ||
       request_body[6] != AIMEE_RESPONSE_FIELD_COUNT || request_body[7] != 0 ||
       aimee_response_get_u32(request_body + 8) > 1u)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   const uint8_t *fields[AIMEE_RESPONSE_FIELD_COUNT];
   uint32_t lengths[AIMEE_RESPONSE_FIELD_COUNT];
   size_t offset = AIMEE_RESPONSE_REQUEST_HEADER_LEN;
   for (size_t i = 0; i < AIMEE_RESPONSE_FIELD_COUNT; ++i)
   {
      if (offset + 4 > request_len)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      lengths[i] = aimee_response_get_u32(request_body + offset);
      offset += 4;
      if (lengths[i] > request_len - offset)
         return AIMEE_MODULE_STATUS_INVALID_REQUEST;
      fields[i] = request_body + offset;
      offset += lengths[i];
   }
   if (offset != request_len || lengths[0] > 128)
      return AIMEE_MODULE_STATUS_INVALID_REQUEST;
   if (aimee_module_invocation_cancelled(invocation))
      return AIMEE_MODULE_STATUS_CANCELLED;

   uint64_t body_hash = fnv1a(fields[6], lengths[6]);
   uint64_t context_hash = fnv1a(fields[7], lengths[7]);
   uint64_t flags_hash = fnv1a(fields[8], lengths[8]);
   char hashes[56];
   snprintf(hashes, sizeof(hashes), "%016llx%016llx%016llx",
            (unsigned long long)body_hash, (unsigned long long)context_hash,
            (unsigned long long)flags_hash);
   uint64_t first = 1469598103934665603ULL, second = 0x84222325cbf29ce4ULL;
   for (size_t i = 1; i <= 4; ++i)
   {
      first = fnv_field(first, 0, fields[i], lengths[i]);
      second = fnv_field(second, 0x9e3779b97f4a7c15ULL, fields[i], lengths[i]);
   }
   const uint8_t stream = aimee_response_get_u32(request_body + 8) ? '1' : '0';
   first = fnv_field(first, 0, &stream, 1);
   second = fnv_field(second, 0x9e3779b97f4a7c15ULL, &stream, 1);
   first = fnv_field(first, 0, fields[5], lengths[5]);
   second = fnv_field(second, 0x9e3779b97f4a7c15ULL, fields[5], lengths[5]);
   first = fnv_field(first, 0, (const uint8_t *)hashes, strlen(hashes));
   second = fnv_field(second, 0x9e3779b97f4a7c15ULL, (const uint8_t *)hashes, strlen(hashes));

   char principal[129];
   if (lengths[0])
      memcpy(principal, fields[0], lengths[0]);
   principal[lengths[0]] = '\0';
   if (!principal[0])
      snprintf(principal, sizeof(principal), "anon");
   char key[AIMEE_RESPONSE_KEY_MAX + 1];
   int key_len = snprintf(key, sizeof(key), "%s|%016llx%016llx", principal,
                          (unsigned long long)first, (unsigned long long)second);
   if (key_len <= 0 || key_len > (int)AIMEE_RESPONSE_KEY_MAX ||
       (uint32_t)key_len + 4u > response_capacity)
      return AIMEE_MODULE_STATUS_INTERNAL;
   aimee_response_put_u32(response_body, AIMEE_RESPONSE_RESPONSE_MAGIC);
   memcpy(response_body + 4, key, (size_t)key_len);
   *response_len = (uint32_t)key_len + 4u;
   return AIMEE_MODULE_STATUS_OK;
}
