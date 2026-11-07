/* Wire contract for tool-dispatch classification. */
#ifndef AIMEE_TOOLS_MODULE_API_H
#define AIMEE_TOOLS_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_TOOLS_EVENT_ACTION_RESOURCE 6914u
#define AIMEE_TOOLS_STAGE_ACTION_RESOURCE 2u
#define AIMEE_TOOLS_EVENT_DISPATCH        6913u
#define AIMEE_TOOLS_STAGE_DISPATCH        1u
#define AIMEE_TOOLS_REQUEST_MAGIC         0x53494454u /* "TDIS" */
#define AIMEE_TOOLS_RESPONSE_MAGIC        0x534c4354u /* "TCLS" */
#define AIMEE_TOOLS_WIRE_VERSION          1u
#define AIMEE_TOOLS_NAME_MAX              95u
#define AIMEE_TOOLS_REQUEST_LEN           104u
#define AIMEE_TOOLS_RESPONSE_LEN          8u
/* Version 1 classification remains byte-compatible. Version 2 requests on the
 * same owner/stage return policy metadata, never an authorization decision. */
#define AIMEE_TOOLS_METADATA_VERSION     2u
#define AIMEE_TOOLS_METADATA_MAGIC       0x4154454du /* "META" */
#define AIMEE_TOOLS_METADATA_LEN         16u
#define AIMEE_TOOLS_METADATA_NAME_MAX    4095u
#define AIMEE_TOOLS_METADATA_REQUEST_MAX (8u + AIMEE_TOOLS_METADATA_NAME_MAX)
#define AIMEE_TOOL_POLICY_READ           (1u << 0)
#define AIMEE_TOOL_POLICY_WRITE          (1u << 1)
#define AIMEE_TOOL_POLICY_FILESYSTEM     (1u << 2)
#define AIMEE_TOOL_POLICY_NETWORK        (1u << 3)
#define AIMEE_TOOL_POLICY_EXTERNAL       (1u << 4)
#define AIMEE_TOOL_POLICY_EXECUTION      (1u << 5)
#define AIMEE_TOOL_POLICY_CONTROL        (1u << 6)
#define AIMEE_TOOL_POLICY_ALL            ((1u << 7) - 1u)

typedef enum
{
   AIMEE_TOOL_CLASS_UNKNOWN = 0,
   AIMEE_TOOL_CLASS_READ = 1,
   AIMEE_TOOL_CLASS_WRITE = 2,
   AIMEE_TOOL_CLASS_EXEC = 3,
   AIMEE_TOOL_CLASS_CONTROL = 4,
   AIMEE_TOOL_CLASS_REMOTE = 5
} aimee_tool_class_t;

typedef struct
{
   aimee_tool_class_t classification;
   uint32_t primary;
   uint32_t categories;
} aimee_tool_metadata_t;

static inline int aimee_tool_metadata_valid(const aimee_tool_metadata_t *value)
{
   return value && value->classification >= AIMEE_TOOL_CLASS_READ &&
          value->classification <= AIMEE_TOOL_CLASS_REMOTE && value->primary &&
          !(value->primary & (value->primary - 1u)) &&
          !(value->categories & ~AIMEE_TOOL_POLICY_ALL) &&
          (value->categories & value->primary) == value->primary;
}

static inline const char *aimee_tool_policy_name(uint32_t category)
{
   switch (category)
   {
   case AIMEE_TOOL_POLICY_READ:
      return "read";
   case AIMEE_TOOL_POLICY_WRITE:
      return "write";
   case AIMEE_TOOL_POLICY_FILESYSTEM:
      return "filesystem";
   case AIMEE_TOOL_POLICY_NETWORK:
      return "network";
   case AIMEE_TOOL_POLICY_EXTERNAL:
      return "external";
   case AIMEE_TOOL_POLICY_EXECUTION:
      return "execution";
   case AIMEE_TOOL_POLICY_CONTROL:
      return "control";
   default:
      return NULL;
   }
}

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

static inline int aimee_tools_request_decode(const uint8_t *in, size_t len, char *name, size_t cap)
{
   if (!in || len != AIMEE_TOOLS_REQUEST_LEN || !name || cap == 0 ||
       aimee_tools_get_u32(in) != AIMEE_TOOLS_REQUEST_MAGIC || in[4] != AIMEE_TOOLS_WIRE_VERSION ||
       in[5] != 0 || in[7] != 0 || in[6] == 0 || in[6] > AIMEE_TOOLS_NAME_MAX ||
       (size_t)in[6] >= cap)
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

/* Version 2 uses a complete length-prefixed name, without v1's 95-byte cap or
 * implicit zero padding. Unknown and over-limit names never alias a prefix. */
static inline size_t aimee_tools_metadata_request_encode(const char *name, uint8_t *out, size_t cap)
{
   size_t length = name ? strlen(name) : 0;
   if (!out || !length || length > AIMEE_TOOLS_METADATA_NAME_MAX || cap < length + 8u)
      return 0;
   memset(out, 0, 8);
   aimee_tools_put_u32(out, AIMEE_TOOLS_REQUEST_MAGIC);
   out[4] = AIMEE_TOOLS_METADATA_VERSION;
   out[6] = (uint8_t)length;
   out[7] = (uint8_t)(length >> 8);
   memcpy(out + 8, name, length);
   return length + 8u;
}

static inline int aimee_tools_metadata_request_decode(const uint8_t *in, size_t length, char *name,
                                                      size_t cap)
{
   if (!in || length < 9 || !name || !cap || aimee_tools_get_u32(in) != AIMEE_TOOLS_REQUEST_MAGIC ||
       in[4] != AIMEE_TOOLS_METADATA_VERSION || in[5])
      return -1;
   size_t size = (size_t)in[6] | ((size_t)in[7] << 8);
   if (!size || size > AIMEE_TOOLS_METADATA_NAME_MAX || length != size + 8u || size >= cap ||
       memchr(in + 8, 0, size))
      return -1;
   memcpy(name, in + 8, size);
   name[size] = 0;
   return 0;
}

static inline int aimee_tools_metadata_response_decode(const uint8_t *in, size_t len,
                                                       aimee_tool_metadata_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   if (!in || len != AIMEE_TOOLS_METADATA_LEN ||
       aimee_tools_get_u32(in) != AIMEE_TOOLS_METADATA_MAGIC)
      return -1;
   aimee_tool_metadata_t value = {(aimee_tool_class_t)aimee_tools_get_u32(in + 4),
                                  aimee_tools_get_u32(in + 8), aimee_tools_get_u32(in + 12)};
   if (!aimee_tool_metadata_valid(&value))
      return -1;
   *out = value;
   return 0;
}

#endif
