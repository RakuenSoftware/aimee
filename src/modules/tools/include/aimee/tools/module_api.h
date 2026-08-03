/* Wire contract for tool-dispatch classification. */
#ifndef AIMEE_TOOLS_MODULE_API_H
#define AIMEE_TOOLS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_TOOLS_EVENT_DISPATCH   6913u
#define AIMEE_TOOLS_STAGE_DISPATCH   1u
#define AIMEE_TOOLS_REQUEST_MAGIC    0x53494454u /* "TDIS" */
#define AIMEE_TOOLS_RESPONSE_MAGIC   0x534c4354u /* "TCLS" */
#define AIMEE_TOOLS_WIRE_VERSION     1u
#define AIMEE_TOOLS_NAME_MAX         95u
#define AIMEE_TOOLS_REQUEST_LEN      104u
#define AIMEE_TOOLS_RESPONSE_LEN     8u

typedef enum
{
   AIMEE_TOOL_CLASS_UNKNOWN = 0,
   AIMEE_TOOL_CLASS_READ = 1,
   AIMEE_TOOL_CLASS_WRITE = 2,
   AIMEE_TOOL_CLASS_EXEC = 3,
   AIMEE_TOOL_CLASS_CONTROL = 4,
   AIMEE_TOOL_CLASS_REMOTE = 5
} aimee_tool_class_t;

static inline void aimee_tools_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_tools_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline int aimee_tools_request_encode(const char *name, uint8_t *out, size_t cap)
{
   size_t len = name ? strlen(name) : 0;
   if (!out || cap < AIMEE_TOOLS_REQUEST_LEN || len == 0 || len > AIMEE_TOOLS_NAME_MAX)
      return -1;
   memset(out, 0, AIMEE_TOOLS_REQUEST_LEN);
   aimee_tools_put_u32(out, AIMEE_TOOLS_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_TOOLS_WIRE_VERSION;
   out[6] = (uint8_t)len;
   memcpy(out + 8, name, len);
   return 0;
}

static inline int aimee_tools_request_decode(const uint8_t *in, size_t len, char *name,
                                              size_t cap)
{
   if (!in || len != AIMEE_TOOLS_REQUEST_LEN || !name || cap == 0 ||
       aimee_tools_get_u32(in) != AIMEE_TOOLS_REQUEST_MAGIC ||
       in[4] != AIMEE_TOOLS_WIRE_VERSION || in[5] != 0 || in[7] != 0 || in[6] == 0 ||
       in[6] > AIMEE_TOOLS_NAME_MAX || (size_t)in[6] >= cap)
      return -1;
   memcpy(name, in + 8, in[6]);
   name[in[6]] = '\0';
   return 0;
}

static inline int aimee_tools_response_decode(const uint8_t *in, size_t len,
                                               aimee_tool_class_t *classification)
{
   if (!in || len != AIMEE_TOOLS_RESPONSE_LEN || !classification ||
       aimee_tools_get_u32(in) != AIMEE_TOOLS_RESPONSE_MAGIC)
      return -1;
   uint32_t value = aimee_tools_get_u32(in + 4);
   if (value > AIMEE_TOOL_CLASS_REMOTE)
      return -1;
   *classification = (aimee_tool_class_t)value;
   return 0;
}

#endif
