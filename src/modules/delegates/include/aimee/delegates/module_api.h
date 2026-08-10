/* Wire contract for delegate invocation role normalization. */
#ifndef AIMEE_DELEGATES_MODULE_API_H
#define AIMEE_DELEGATES_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_DELEGATES_EVENT_INVOKE   6657u
#define AIMEE_DELEGATES_STAGE_INVOKE   1u
#define AIMEE_DELEGATES_REQUEST_MAGIC  0x4c4f5244u /* "DROL" */
#define AIMEE_DELEGATES_RESPONSE_MAGIC 0x4e414344u /* "DCAN" */
#define AIMEE_DELEGATES_WIRE_VERSION   1u
#define AIMEE_DELEGATES_ROLE_MAX       63u
#define AIMEE_DELEGATES_MESSAGE_LEN    72u

/* Capability inference: what a prompt implies a model must be able to do. */
#define AIMEE_DELEGATES_EVENT_CAPABILITIES 6658u
#define AIMEE_DELEGATES_STAGE_CAPABILITIES 2u
#define AIMEE_DELEGATES_CAP_REQUEST_MAGIC  0x50414344u /* "DCAP" */
#define AIMEE_DELEGATES_CAP_RESPONSE_MAGIC 0x53414344u /* "DCAS" */
#define AIMEE_DELEGATES_CAP_HEADER_LEN     12u
#define AIMEE_DELEGATES_CAP_RESPONSE_LEN   12u
#define AIMEE_DELEGATES_CAP_PROMPT_MAX     (1u << 20)

/* Mirrors model_registry.h. A model capability is a property of the model, so
 * the numbering belongs to the registry and is restated here only so the wire
 * has a definition that does not depend on server headers. */
#define AIMEE_DELEGATES_CAP_TOOLS  (1u << 1)
#define AIMEE_DELEGATES_CAP_VISION (1u << 2)
#define AIMEE_DELEGATES_CAP_PDF    (1u << 3)
#define AIMEE_DELEGATES_CAP_AUDIO  (1u << 4)

static inline void aimee_delegates_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_delegates_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline int aimee_delegates_message_encode(uint32_t magic, const char *role, uint8_t *out,
                                                 size_t cap)
{
   size_t len = role ? strlen(role) : 0;
   if (!out || cap < AIMEE_DELEGATES_MESSAGE_LEN || len == 0 || len > AIMEE_DELEGATES_ROLE_MAX)
      return -1;
   memset(out, 0, AIMEE_DELEGATES_MESSAGE_LEN);
   aimee_delegates_put_u32(out, magic);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[6] = (uint8_t)len;
   memcpy(out + 8, role, len);
   return 0;
}

static inline int aimee_delegates_message_decode(const uint8_t *in, size_t len, uint32_t magic,
                                                 char *role, size_t role_cap)
{
   if (!in || len != AIMEE_DELEGATES_MESSAGE_LEN || !role || role_cap == 0 ||
       aimee_delegates_get_u32(in) != magic || in[4] != AIMEE_DELEGATES_WIRE_VERSION ||
       in[5] != 0 || in[7] != 0 || in[6] == 0 || in[6] > AIMEE_DELEGATES_ROLE_MAX ||
       (size_t)in[6] >= role_cap)
      return -1;
   memcpy(role, in + 8, in[6]);
   role[in[6]] = '\0';
   return 0;
}

/* Frame a prompt for capability inference. Returns the encoded length, or 0
 * when it does not fit. A prompt is carried whole because the rule reads its
 * text; there is nothing smaller to send that preserves the answer. */
static inline size_t aimee_delegates_cap_request_encode(const char *prompt, size_t prompt_len,
                                                        int tools_enabled, uint8_t *out, size_t cap)
{
   if (!out || prompt_len > AIMEE_DELEGATES_CAP_PROMPT_MAX ||
       cap < AIMEE_DELEGATES_CAP_HEADER_LEN + prompt_len)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_CAP_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_CAP_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = tools_enabled ? 1u : 0u;
   aimee_delegates_put_u32(out + 8, (uint32_t)prompt_len);
   if (prompt_len)
      memcpy(out + AIMEE_DELEGATES_CAP_HEADER_LEN, prompt, prompt_len);
   return AIMEE_DELEGATES_CAP_HEADER_LEN + prompt_len;
}

static inline int aimee_delegates_cap_response_decode(const uint8_t *in, size_t len,
                                                      unsigned *required_caps, int *min_context)
{
   if (!in || len != AIMEE_DELEGATES_CAP_RESPONSE_LEN ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_CAP_RESPONSE_MAGIC)
      return -1;
   if (required_caps)
      *required_caps = (unsigned)aimee_delegates_get_u32(in + 4);
   if (min_context)
      *min_context = (int)aimee_delegates_get_u32(in + 8);
   return 0;
}

#endif
