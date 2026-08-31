/* Wire contract for control-web's bounded proxy-route authorization. */
#ifndef AIMEE_CONTROL_WEB_MODULE_API_H
#define AIMEE_CONTROL_WEB_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_CONTROL_WEB_EVENT_AUTHORIZE 10241u
#define AIMEE_CONTROL_WEB_STAGE_AUTHORIZE 1u
#define AIMEE_CONTROL_WEB_REQUEST_MAGIC 0x51455743u /* "CWEQ" */
#define AIMEE_CONTROL_WEB_RESPONSE_MAGIC 0x53455743u /* "CWES" */
#define AIMEE_CONTROL_WEB_WIRE_VERSION 1u
#define AIMEE_CONTROL_WEB_METHOD_MAX 15u
#define AIMEE_CONTROL_WEB_PATH_MAX 511u
#define AIMEE_CONTROL_WEB_REQUEST_METHOD_OFF 32u
#define AIMEE_CONTROL_WEB_REQUEST_PATH_OFF 48u
#define AIMEE_CONTROL_WEB_REQUEST_LEN 560u
#define AIMEE_CONTROL_WEB_RESPONSE_LEN 16u

typedef enum
{
   AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN = 1,
   AIMEE_CONTROL_WEB_TARGET_FLEET = 2,
} aimee_control_web_target_t;

static inline void aimee_control_web_put_u32(uint8_t *p, uint32_t value)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(value >> (i * 8u));
}

static inline uint32_t aimee_control_web_get_u32(const uint8_t *p)
{
   uint32_t value = 0;
   for (unsigned i = 0; i < 4; ++i)
      value |= (uint32_t)p[i] << (i * 8u);
   return value;
}

static inline int aimee_control_web_zero_padding(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] != 0)
         return 0;
   return 1;
}

static inline int aimee_control_web_nonzero_text(const uint8_t *p, size_t len)
{
   for (size_t i = 0; i < len; ++i)
      if (p[i] == 0)
         return 0;
   return 1;
}

static inline int aimee_control_web_target_valid(uint32_t target)
{
   return target == AIMEE_CONTROL_WEB_TARGET_CONSOLE_ADMIN ||
          target == AIMEE_CONTROL_WEB_TARGET_FLEET;
}

static inline int aimee_control_web_request_encode(aimee_control_web_target_t target,
                                                    const char *method, const char *path,
                                                    uint8_t *out, size_t capacity)
{
   const char *method_value = method ? method : "";
   const char *path_value = path ? path : "";
   size_t method_len = strlen(method_value), path_len = strlen(path_value);
   if (!out || capacity < AIMEE_CONTROL_WEB_REQUEST_LEN ||
       !aimee_control_web_target_valid((uint32_t)target) ||
       method_len > AIMEE_CONTROL_WEB_METHOD_MAX || path_len > AIMEE_CONTROL_WEB_PATH_MAX)
      return -1;
   memset(out, 0, AIMEE_CONTROL_WEB_REQUEST_LEN);
   aimee_control_web_put_u32(out, AIMEE_CONTROL_WEB_REQUEST_MAGIC);
   aimee_control_web_put_u32(out + 4, AIMEE_CONTROL_WEB_WIRE_VERSION);
   aimee_control_web_put_u32(out + 8, (uint32_t)target);
   aimee_control_web_put_u32(out + 12, (uint32_t)method_len);
   aimee_control_web_put_u32(out + 16, (uint32_t)path_len);
   if (method_len)
      memcpy(out + AIMEE_CONTROL_WEB_REQUEST_METHOD_OFF, method_value, method_len);
   if (path_len)
      memcpy(out + AIMEE_CONTROL_WEB_REQUEST_PATH_OFF, path_value, path_len);
   return 0;
}

static inline int aimee_control_web_request_decode(
    const uint8_t *in, size_t len, aimee_control_web_target_t *target,
    char method[AIMEE_CONTROL_WEB_METHOD_MAX + 1u],
    char path[AIMEE_CONTROL_WEB_PATH_MAX + 1u])
{
   if (!in || len != AIMEE_CONTROL_WEB_REQUEST_LEN || !target || !method || !path ||
       aimee_control_web_get_u32(in) != AIMEE_CONTROL_WEB_REQUEST_MAGIC ||
       aimee_control_web_get_u32(in + 4) != AIMEE_CONTROL_WEB_WIRE_VERSION ||
       !aimee_control_web_target_valid(aimee_control_web_get_u32(in + 8)) ||
       aimee_control_web_get_u32(in + 12) > AIMEE_CONTROL_WEB_METHOD_MAX ||
       aimee_control_web_get_u32(in + 16) > AIMEE_CONTROL_WEB_PATH_MAX ||
       aimee_control_web_get_u32(in + 20) != 0 ||
       aimee_control_web_get_u32(in + 24) != 0 || aimee_control_web_get_u32(in + 28) != 0)
      return -1;
   uint32_t method_len = aimee_control_web_get_u32(in + 12);
   uint32_t path_len = aimee_control_web_get_u32(in + 16);
   const uint8_t *method_slot = in + AIMEE_CONTROL_WEB_REQUEST_METHOD_OFF;
   const uint8_t *path_slot = in + AIMEE_CONTROL_WEB_REQUEST_PATH_OFF;
   if (!aimee_control_web_nonzero_text(method_slot, method_len) ||
       !aimee_control_web_zero_padding(method_slot + method_len,
                                       AIMEE_CONTROL_WEB_METHOD_MAX + 1u - method_len) ||
       !aimee_control_web_nonzero_text(path_slot, path_len) ||
       !aimee_control_web_zero_padding(path_slot + path_len,
                                       AIMEE_CONTROL_WEB_PATH_MAX + 1u - path_len))
      return -1;
   if (method_len)
      memcpy(method, method_slot, method_len);
   method[method_len] = '\0';
   if (path_len)
      memcpy(path, path_slot, path_len);
   path[path_len] = '\0';
   *target = (aimee_control_web_target_t)aimee_control_web_get_u32(in + 8);
   return 0;
}

static inline int aimee_control_web_response_encode(int allowed, uint8_t *out,
                                                     size_t capacity)
{
   if (!out || capacity < AIMEE_CONTROL_WEB_RESPONSE_LEN || (allowed != 0 && allowed != 1))
      return -1;
   memset(out, 0, AIMEE_CONTROL_WEB_RESPONSE_LEN);
   aimee_control_web_put_u32(out, AIMEE_CONTROL_WEB_RESPONSE_MAGIC);
   aimee_control_web_put_u32(out + 4, AIMEE_CONTROL_WEB_WIRE_VERSION);
   aimee_control_web_put_u32(out + 8, (uint32_t)allowed);
   return 0;
}

static inline int aimee_control_web_response_decode(const uint8_t *in, size_t len,
                                                     int *allowed)
{
   if (!in || len != AIMEE_CONTROL_WEB_RESPONSE_LEN || !allowed ||
       aimee_control_web_get_u32(in) != AIMEE_CONTROL_WEB_RESPONSE_MAGIC ||
       aimee_control_web_get_u32(in + 4) != AIMEE_CONTROL_WEB_WIRE_VERSION ||
       aimee_control_web_get_u32(in + 8) > 1 || aimee_control_web_get_u32(in + 12) != 0)
      return -1;
   *allowed = aimee_control_web_get_u32(in + 8) == 1;
   return 0;
}

#endif
