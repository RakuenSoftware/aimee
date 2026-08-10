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

/* Chain depth: how deep a delegation may nest, and when an inherited depth is
 * stale. Depth crosses process boundaries in an environment variable; reading
 * and writing it is the caller's business, what it implies is the module's. */
#define AIMEE_DELEGATES_EVENT_CHAIN           6659u
#define AIMEE_DELEGATES_STAGE_CHAIN           3u
#define AIMEE_DELEGATES_CHAIN_REQUEST_MAGIC   0x4e484344u /* "DCHN" */
#define AIMEE_DELEGATES_CHAIN_RESPONSE_MAGIC  0x52484344u /* "DCHR" */
#define AIMEE_DELEGATES_CHAIN_REQUEST_LEN     20u
#define AIMEE_DELEGATES_CHAIN_RESPONSE_LEN    12u
#define AIMEE_DELEGATES_CHAIN_OP_SHOULD_CLEAR 1u
#define AIMEE_DELEGATES_CHAIN_OP_CHECK_DEPTH  2u

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

/* Frame a chain question. Flags are booleans and must be 0 or 1; parent_depth
 * and max_depth are only read by the depth op. */
static inline int aimee_delegates_chain_request_encode(unsigned op, int has_depth, int has_parent,
                                                       int parent_known, int parent_active,
                                                       int32_t parent_depth, int32_t max_depth,
                                                       uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_CHAIN_REQUEST_LEN)
      return -1;
   memset(out, 0, AIMEE_DELEGATES_CHAIN_REQUEST_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_CHAIN_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = (uint8_t)op;
   out[6] = has_depth ? 1u : 0u;
   out[7] = has_parent ? 1u : 0u;
   out[8] = parent_known ? 1u : 0u;
   out[9] = parent_active ? 1u : 0u;
   aimee_delegates_put_u32(out + 12, (uint32_t)parent_depth);
   aimee_delegates_put_u32(out + 16, (uint32_t)max_depth);
   return 0;
}

/* `flag` is the op's boolean answer: should-clear, or depth-allowed. */
static inline int aimee_delegates_chain_response_decode(const uint8_t *in, size_t len, int *flag,
                                                        int32_t *current_depth)
{
   if (!in || len != AIMEE_DELEGATES_CHAIN_RESPONSE_LEN ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_CHAIN_RESPONSE_MAGIC || in[4] > 1u)
      return -1;
   if (flag)
      *flag = in[4] == 1u;
   if (current_depth)
      *current_depth = (int32_t)aimee_delegates_get_u32(in + 8);
   return 0;
}

/* Named-path extraction: which repo files a brief names as targets. The rule
 * lives only in the Go module -- it is a long scan and a second copy would be
 * drift waiting to happen -- so there is no C mirror and no parity fixture. */
#define AIMEE_DELEGATES_EVENT_PATHS           6660u
#define AIMEE_DELEGATES_STAGE_PATHS           4u
#define AIMEE_DELEGATES_PATHS_REQUEST_MAGIC   0x54415044u /* "DPAT" */
#define AIMEE_DELEGATES_PATHS_RESPONSE_MAGIC  0x53415044u /* "DPAS" */
#define AIMEE_DELEGATES_PATHS_HEADER_LEN      12u
#define AIMEE_DELEGATES_PATHS_RESP_HEADER_LEN 8u
#define AIMEE_DELEGATES_PATHS_PROMPT_MAX      (1u << 20)

static inline size_t aimee_delegates_paths_request_encode(const char *prompt, size_t prompt_len,
                                                          unsigned max_paths, uint8_t *out,
                                                          size_t cap)
{
   if (!out || max_paths == 0 || max_paths > 255 || prompt_len > AIMEE_DELEGATES_PATHS_PROMPT_MAX ||
       cap < AIMEE_DELEGATES_PATHS_HEADER_LEN + prompt_len)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_PATHS_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_PATHS_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = (uint8_t)max_paths;
   aimee_delegates_put_u32(out + 8, (uint32_t)prompt_len);
   if (prompt_len)
      memcpy(out + AIMEE_DELEGATES_PATHS_HEADER_LEN, prompt, prompt_len);
   return AIMEE_DELEGATES_PATHS_HEADER_LEN + prompt_len;
}

/* Copy the returned paths into `paths`, each at most path_stride bytes
 * including the terminator. Returns the count written, or -1 on a malformed
 * response. Each path is length-prefixed on the wire, so nothing here scans for
 * a terminator it would have to trust. */
static inline int aimee_delegates_paths_response_decode(const uint8_t *in, size_t len, char *paths,
                                                        size_t path_stride, unsigned max_paths)
{
   if (!in || len < AIMEE_DELEGATES_PATHS_RESP_HEADER_LEN || !paths || path_stride == 0 ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_PATHS_RESPONSE_MAGIC)
      return -1;
   uint32_t count = aimee_delegates_get_u32(in + 4);
   if (count > max_paths)
      return -1;
   size_t at = AIMEE_DELEGATES_PATHS_RESP_HEADER_LEN;
   for (uint32_t i = 0; i < count; ++i)
   {
      if (at + 2u > len)
         return -1;
      size_t n = (size_t)in[at] | ((size_t)in[at + 1] << 8);
      at += 2u;
      if (at + n > len || n + 1u > path_stride)
         return -1;
      memcpy(paths + (size_t)i * path_stride, in + at, n);
      paths[(size_t)i * path_stride + n] = '\0';
      at += n;
   }
   return (int)count;
}

/* Handoff validation: whether a delegate's structured report can be believed.
 * The rule lives only in the Go module; there is no C mirror. */
#define AIMEE_DELEGATES_EVENT_HANDOFF          6661u
#define AIMEE_DELEGATES_STAGE_HANDOFF          5u
#define AIMEE_DELEGATES_HANDOFF_REQUEST_MAGIC  0x444e4844u /* "DHND" */
#define AIMEE_DELEGATES_HANDOFF_RESPONSE_MAGIC 0x564e4844u /* "DHNV" */
#define AIMEE_DELEGATES_HANDOFF_HEADER_LEN     16u
#define AIMEE_DELEGATES_HANDOFF_STATUS_LEN     32u
#define AIMEE_DELEGATES_HANDOFF_ERROR_LEN      256u
#define AIMEE_DELEGATES_HANDOFF_RESPONSE_LEN                                                       \
   (4u + 8u * 4u + AIMEE_DELEGATES_HANDOFF_STATUS_LEN * 2u + AIMEE_DELEGATES_HANDOFF_ERROR_LEN)
#define AIMEE_DELEGATES_HANDOFF_TEXT_MAX (1u << 20)

static inline size_t aimee_delegates_handoff_request_encode(const char *text, size_t text_len,
                                                            const char *owned, size_t owned_len,
                                                            int require_verification, uint8_t *out,
                                                            size_t cap)
{
   if (!out || text_len > AIMEE_DELEGATES_HANDOFF_TEXT_MAX ||
       owned_len > AIMEE_DELEGATES_HANDOFF_TEXT_MAX ||
       cap < AIMEE_DELEGATES_HANDOFF_HEADER_LEN + text_len + owned_len)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_HANDOFF_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_HANDOFF_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = require_verification ? 1u : 0u;
   aimee_delegates_put_u32(out + 8, (uint32_t)text_len);
   aimee_delegates_put_u32(out + 12, (uint32_t)owned_len);
   if (text_len)
      memcpy(out + AIMEE_DELEGATES_HANDOFF_HEADER_LEN, text, text_len);
   if (owned_len)
      memcpy(out + AIMEE_DELEGATES_HANDOFF_HEADER_LEN + text_len, owned, owned_len);
   return AIMEE_DELEGATES_HANDOFF_HEADER_LEN + text_len + owned_len;
}

/* Copy one fixed-width, NUL-padded field out of the response. */
static inline void aimee_delegates_handoff_field(const uint8_t *in, size_t at, size_t width,
                                                 char *out, size_t cap)
{
   size_t n = 0;
   while (n < width && in[at + n] != 0)
      ++n;
   if (n >= cap)
      n = cap ? cap - 1 : 0;
   if (cap)
   {
      memcpy(out, in + at, n);
      out[n] = '\0';
   }
}

/* --- Tool-call rescue (stage 6) --- */

#define AIMEE_DELEGATES_EVENT_RESCUE          6662u
#define AIMEE_DELEGATES_STAGE_RESCUE          6u
#define AIMEE_DELEGATES_RESCUE_REQUEST_MAGIC  0x51535244u /* "DRSQ" */
#define AIMEE_DELEGATES_RESCUE_RESPONSE_MAGIC 0x52535244u /* "DRSR" */
#define AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN 16u
#define AIMEE_DELEGATES_RESCUE_RESP_HEADER_LEN 16u
#define AIMEE_DELEGATES_RESCUE_TEXT_MAX       (1u << 20)
#define AIMEE_DELEGATES_RESCUE_KNOWN_MAX      4096u
#define AIMEE_DELEGATES_RESCUE_MODE_PARSE     0u
#define AIMEE_DELEGATES_RESCUE_MODE_DETECT    1u

/* Encode a rescue request: the response text, then the caller's tool
 * inventory as u16-length-prefixed names. The inventory travels with the
 * request because whether a rescued name is real is the caller's knowledge,
 * not something the module may go and ask another module for. */
static inline size_t aimee_delegates_rescue_request_encode(const char *text, size_t text_len,
                                                           const char *const *names, size_t name_count,
                                                           int allow_json, unsigned mode,
                                                           uint8_t *out, size_t cap)
{
   size_t at, i;
   if (!out || text_len > AIMEE_DELEGATES_RESCUE_TEXT_MAX ||
       name_count > AIMEE_DELEGATES_RESCUE_KNOWN_MAX ||
       cap < AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN + text_len)
      return 0;

   memset(out, 0, AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_RESCUE_REQUEST_MAGIC);
   out[4] = 1; /* wire version */
   out[5] = allow_json ? 1 : 0;
   out[6] = (uint8_t)mode;
   aimee_delegates_put_u32(out + 8, (uint32_t)text_len);
   aimee_delegates_put_u32(out + 12, (uint32_t)name_count);
   if (text_len)
      memcpy(out + AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN, text, text_len);

   at = AIMEE_DELEGATES_RESCUE_REQ_HEADER_LEN + text_len;
   for (i = 0; i < name_count; i++)
   {
      size_t n = names[i] ? strlen(names[i]) : 0;
      if (n > 0xffffu || at + 2 + n > cap)
         return 0;
      out[at] = (uint8_t)(n & 0xffu);
      out[at + 1] = (uint8_t)((n >> 8) & 0xffu);
      at += 2;
      memcpy(out + at, names[i], n);
      at += n;
   }
   return at;
}

#endif
