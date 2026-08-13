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

/* --- Verification outcome and escalation policy (stage 7) --- */

#define AIMEE_DELEGATES_EVENT_VERIFY          6663u
#define AIMEE_DELEGATES_STAGE_VERIFY          7u
#define AIMEE_DELEGATES_VERIFY_REQUEST_MAGIC  0x51524556u /* "VERQ" */
#define AIMEE_DELEGATES_VERIFY_RESPONSE_MAGIC 0x53524556u /* "VERS" */
#define AIMEE_DELEGATES_VERIFY_REQUEST_LEN    20u
#define AIMEE_DELEGATES_VERIFY_RESPONSE_LEN   12u

static inline int aimee_delegates_verify_request_encode(unsigned op, int a, int b,
                                                        int max_signal_status, uint8_t *out,
                                                        size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_VERIFY_REQUEST_LEN)
      return -1;
   memset(out, 0, AIMEE_DELEGATES_VERIFY_REQUEST_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_VERIFY_REQUEST_MAGIC);
   out[4] = 1; /* wire version */
   out[5] = (uint8_t)op;
   aimee_delegates_put_u32(out + 8, (uint32_t)a);
   aimee_delegates_put_u32(out + 12, (uint32_t)b);
   aimee_delegates_put_u32(out + 16, (uint32_t)max_signal_status);
   return 0;
}

/* --- Delegate-run economics (stage 8) --- */

#define AIMEE_DELEGATES_EVENT_ECONOMICS          6664u
#define AIMEE_DELEGATES_STAGE_ECONOMICS          8u
#define AIMEE_DELEGATES_ECON_REQUEST_MAGIC       0x51434544u /* "DECQ" */
#define AIMEE_DELEGATES_ECON_RESPONSE_MAGIC      0x53434544u /* "DECS" */
#define AIMEE_DELEGATES_ECON_REQ_HEADER_LEN      16u
#define AIMEE_DELEGATES_ECON_VERDICT_LEN         32u
#define AIMEE_DELEGATES_ECON_ADVICE_LEN          256u
#define AIMEE_DELEGATES_ECON_LABEL_LEN           64u
#define AIMEE_DELEGATES_ECON_FIELD_COUNT         19u
#define AIMEE_DELEGATES_ECON_RESPONSE_LEN                                                          \
   (4u + AIMEE_DELEGATES_ECON_FIELD_COUNT * 4u + AIMEE_DELEGATES_ECON_VERDICT_LEN +                \
    AIMEE_DELEGATES_ECON_ADVICE_LEN + 2u * AIMEE_DELEGATES_ECON_LABEL_LEN)
#define AIMEE_DELEGATES_ECON_MAX_TASKS  4096u
#define AIMEE_DELEGATES_ECON_MAX_AGENTS 4096u

/* Start a request; tasks and agents are appended with the helpers below. */
static inline size_t aimee_delegates_econ_request_begin(uint32_t task_count, uint32_t agent_count,
                                                        uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_ECON_REQ_HEADER_LEN ||
       task_count > AIMEE_DELEGATES_ECON_MAX_TASKS ||
       agent_count > AIMEE_DELEGATES_ECON_MAX_AGENTS)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_ECON_REQ_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_ECON_REQUEST_MAGIC);
   out[4] = 1; /* wire version */
   aimee_delegates_put_u32(out + 8, task_count);
   aimee_delegates_put_u32(out + 12, agent_count);
   return AIMEE_DELEGATES_ECON_REQ_HEADER_LEN;
}

static inline size_t aimee_delegates_econ_put_task(const char *status, const char *claimed_by,
                                                   const char *files, const char *result,
                                                   uint8_t *out, size_t at, size_t cap)
{
   size_t status_len = status ? strlen(status) : 0;
   size_t claimed_len = claimed_by ? strlen(claimed_by) : 0;
   size_t files_len = files ? strlen(files) : 0;
   size_t result_len = result ? strlen(result) : 0;
   if (!out || at == 0 || status_len > 0xffffu || claimed_len > 0xffffu ||
       at + 12 + status_len + claimed_len + files_len + result_len > cap)
      return 0;

   out[at] = (uint8_t)(status_len & 0xffu);
   out[at + 1] = (uint8_t)((status_len >> 8) & 0xffu);
   out[at + 2] = (uint8_t)(claimed_len & 0xffu);
   out[at + 3] = (uint8_t)((claimed_len >> 8) & 0xffu);
   aimee_delegates_put_u32(out + at + 4, (uint32_t)files_len);
   aimee_delegates_put_u32(out + at + 8, (uint32_t)result_len);
   at += 12;
   if (status_len)
      memcpy(out + at, status, status_len);
   at += status_len;
   if (claimed_len)
      memcpy(out + at, claimed_by, claimed_len);
   at += claimed_len;
   if (files_len)
      memcpy(out + at, files, files_len);
   at += files_len;
   if (result_len)
      memcpy(out + at, result, result_len);
   return at + result_len;
}

static inline size_t aimee_delegates_econ_put_agent(const char *name, int tier, uint8_t *out,
                                                    size_t at, size_t cap)
{
   size_t name_len = name ? strlen(name) : 0;
   if (!out || at == 0 || name_len > 0xffffu || at + 2 + name_len + 4 > cap)
      return 0;
   out[at] = (uint8_t)(name_len & 0xffu);
   out[at + 1] = (uint8_t)((name_len >> 8) & 0xffu);
   at += 2;
   if (name_len)
      memcpy(out + at, name, name_len);
   at += name_len;
   aimee_delegates_put_u32(out + at, (uint32_t)tier);
   return at + 4;
}

/* --- Patch coordination (stage 9) --- */

#define AIMEE_DELEGATES_EVENT_PATCH          6665u
#define AIMEE_DELEGATES_STAGE_PATCH          9u
#define AIMEE_DELEGATES_PATCH_REQUEST_MAGIC  0x51435044u /* "DPCQ" */
#define AIMEE_DELEGATES_PATCH_RESPONSE_MAGIC 0x53435044u /* "DPCS" */
#define AIMEE_DELEGATES_PATCH_REQ_HEADER_LEN 16u
#define AIMEE_DELEGATES_PATCH_STATE_LEN      32u
#define AIMEE_DELEGATES_PATCH_NOTE_LEN       256u
#define AIMEE_DELEGATES_PATCH_NEXTCMD_LEN    64u
#define AIMEE_DELEGATES_PATCH_RUN_FIELDS     18u
#define AIMEE_DELEGATES_PATCH_RESP_HEADER_LEN                                                      \
   (4u + AIMEE_DELEGATES_PATCH_RUN_FIELDS * 4u + AIMEE_DELEGATES_PATCH_STATE_LEN +                 \
    AIMEE_DELEGATES_PATCH_NEXTCMD_LEN)
#define AIMEE_DELEGATES_PATCH_TASK_FIELDS 9u
#define AIMEE_DELEGATES_PATCH_TASK_REC_LEN                                                         \
   (AIMEE_DELEGATES_PATCH_TASK_FIELDS * 4u + AIMEE_DELEGATES_PATCH_STATE_LEN * 3u +                \
    AIMEE_DELEGATES_PATCH_NOTE_LEN)
#define AIMEE_DELEGATES_PATCH_MAX_TASKS 64u

static inline size_t aimee_delegates_patch_request_begin(uint32_t task_count, uint8_t *out,
                                                         size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_PATCH_REQ_HEADER_LEN ||
       task_count > AIMEE_DELEGATES_PATCH_MAX_TASKS)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_PATCH_REQ_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_PATCH_REQUEST_MAGIC);
   out[4] = 1; /* wire version */
   aimee_delegates_put_u32(out + 8, task_count);
   return AIMEE_DELEGATES_PATCH_REQ_HEADER_LEN;
}

static inline size_t aimee_delegates_patch_put_task(int id, int step_id, const char *status,
                                                    const char *error, const char *files,
                                                    const char *result, uint8_t *out, size_t at,
                                                    size_t cap)
{
   size_t status_len = status ? strlen(status) : 0;
   size_t error_len = error ? strlen(error) : 0;
   size_t files_len = files ? strlen(files) : 0;
   size_t result_len = result ? strlen(result) : 0;
   if (!out || at == 0 || status_len > 0xffffu || error_len > 0xffffu ||
       at + 20 + status_len + error_len + files_len + result_len > cap)
      return 0;

   aimee_delegates_put_u32(out + at, (uint32_t)id);
   aimee_delegates_put_u32(out + at + 4, (uint32_t)step_id);
   out[at + 8] = (uint8_t)(status_len & 0xffu);
   out[at + 9] = (uint8_t)((status_len >> 8) & 0xffu);
   out[at + 10] = (uint8_t)(error_len & 0xffu);
   out[at + 11] = (uint8_t)((error_len >> 8) & 0xffu);
   aimee_delegates_put_u32(out + at + 12, (uint32_t)files_len);
   aimee_delegates_put_u32(out + at + 16, (uint32_t)result_len);
   at += 20;
   if (status_len)
      memcpy(out + at, status, status_len);
   at += status_len;
   if (error_len)
      memcpy(out + at, error, error_len);
   at += error_len;
   if (files_len)
      memcpy(out + at, files, files_len);
   at += files_len;
   if (result_len)
      memcpy(out + at, result, result_len);
   return at + result_len;
}

/* --- Role policy (stage 10) --- */

#define AIMEE_DELEGATES_EVENT_ROLEPOL          6666u
#define AIMEE_DELEGATES_STAGE_ROLEPOL          10u
#define AIMEE_DELEGATES_ROLEPOL_REQUEST_MAGIC  0x514c5244u /* "DRLQ" */
#define AIMEE_DELEGATES_ROLEPOL_RESPONSE_MAGIC 0x534c5244u /* "DRLS" */
#define AIMEE_DELEGATES_ROLEPOL_REQUEST_LEN    (16u + AIMEE_DELEGATES_ROLE_MAX + 1u)
#define AIMEE_DELEGATES_ROLEPOL_RESPONSE_LEN   32u

/* `holds_tools` is the delegate's resolved `tools` permission, and only the
 * auto-tools answer reads it. It is passed rather than looked up from the role
 * because a role an operator DEFINED is visible only in the resolved set: asking
 * about the role inside the module would answer from the built-in table and hand
 * tools to a role that was defined without them. */
static inline int aimee_delegates_rolepol_request_encode(const char *role, int max_turns,
                                                         int explicit_tools, int holds_tools,
                                                         uint8_t *out, size_t cap)
{
   size_t len = role ? strlen(role) : 0;
   if (!out || cap < AIMEE_DELEGATES_ROLEPOL_REQUEST_LEN || len > AIMEE_DELEGATES_ROLE_MAX)
      return -1;
   memset(out, 0, AIMEE_DELEGATES_ROLEPOL_REQUEST_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_ROLEPOL_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = explicit_tools ? 1u : 0u;
   out[6] = holds_tools ? 1u : 0u;
   aimee_delegates_put_u32(out + 8, (uint32_t)max_turns);
   aimee_delegates_put_u32(out + 12, (uint32_t)len);
   if (len)
      memcpy(out + 16, role, len);
   return 0;
}

/* --- Launch args (stage 12): the command that creates a delegate container ---
 *
 * The module decides the container's shape and its name together and returns
 * the argv. Nothing here re-derives either: past the argv every guarantee is
 * just a flag, and a missing flag is a delegate with a network. */

#define AIMEE_DELEGATES_EVENT_LAUNCH          6668u
#define AIMEE_DELEGATES_STAGE_LAUNCH          12u
#define AIMEE_DELEGATES_LAUNCH_REQUEST_MAGIC  0x514c4144u /* "DALQ" */
#define AIMEE_DELEGATES_LAUNCH_RESPONSE_MAGIC 0x534c4144u /* "DALS" */
#define AIMEE_DELEGATES_LAUNCH_HEADER_LEN     16u
#define AIMEE_DELEGATES_LAUNCH_FLAG_GIT       1u /* the worktree is a git checkout */
#define AIMEE_DELEGATES_LAUNCH_FLAG_WRITES    2u /* this delegate may write it */

/* The caller's facts. Every path is the HOST's; the module translates bind
 * sources into the daemon namespace itself, given `mount_table`. */
typedef struct
{
   int writes_allowed;  /* the CALLER's composed answer, not the role default */
   int is_git_checkout; /* the caller stat'd it; the module does not touch disk */
   const char *repo_root;
   const char *worktree;
   const char *gitdir;
   const char *parent_socket_host;   /* the one outward channel, host side */
   const char *parent_socket_target; /* ...and where it appears inside */
   const char *egress_proxy;
   const char *task_id;
   const char *image;
   const char *workdir;
   const char *mount_table; /* "<dest>\t<source>" per line, or NULL */
   const char *run_as_user; /* "<uid>:<gid>", or NULL to keep the image's */
   /* A delegate with NO repository: a directory aimee made for it. Mutually
    * exclusive with the repository fields above -- the module refuses both. */
   const char *scratch_dir;
   const char *scratch_target;
   const char *const *command; /* NULL-terminated, or NULL for the image's */
} aimee_delegates_launch_spec_t;

static inline size_t aimee_delegates_launch_field(uint8_t *out, size_t cap, size_t at,
                                                  const char *value)
{
   size_t len = value ? strlen(value) : 0;
   if (at + 4 + len > cap)
      return 0;
   aimee_delegates_put_u32(out + at, (uint32_t)len);
   if (len)
      memcpy(out + at + 4, value, len);
   return at + 4 + len;
}

/* Returns the encoded length, or 0 when it does not fit. The field ORDER is the
 * wire contract and must match the module's decoder exactly. */
static inline size_t aimee_delegates_launch_request_encode(
    const aimee_delegates_launch_spec_t *spec, uint8_t *out, size_t cap)
{
   if (!spec || !out || cap < AIMEE_DELEGATES_LAUNCH_HEADER_LEN)
      return 0;
   uint32_t argc = 0;
   if (spec->command)
      while (spec->command[argc])
         argc++;

   memset(out, 0, AIMEE_DELEGATES_LAUNCH_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_LAUNCH_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = (uint8_t)((spec->is_git_checkout ? AIMEE_DELEGATES_LAUNCH_FLAG_GIT : 0u) |
                      (spec->writes_allowed ? AIMEE_DELEGATES_LAUNCH_FLAG_WRITES : 0u));
   aimee_delegates_put_u32(out + 8, argc);

   size_t at = AIMEE_DELEGATES_LAUNCH_HEADER_LEN;
   const char *fields[] = {spec->repo_root,           spec->worktree,
                           spec->gitdir,              spec->parent_socket_host,
                           spec->parent_socket_target, spec->egress_proxy,
                           spec->task_id,             spec->image,
                           spec->workdir,             spec->mount_table,
                           spec->run_as_user,         spec->scratch_dir,
                           spec->scratch_target};
   for (unsigned i = 0; i < sizeof(fields) / sizeof(fields[0]); i++)
   {
      at = aimee_delegates_launch_field(out, cap, at, fields[i]);
      if (at == 0)
         return 0;
   }
   for (uint32_t i = 0; i < argc; i++)
   {
      at = aimee_delegates_launch_field(out, cap, at, spec->command[i]);
      if (at == 0)
         return 0;
   }
   return at;
}

/* Decode the container name and argv. `argv_out` receives pointers INTO `in`,
 * so it stays valid only while the response buffer does — no allocation, and
 * nothing to free. Returns the argument count, or -1. */
static inline int aimee_delegates_launch_response_decode(const uint8_t *in, size_t len,
                                                         char *name_out, size_t name_cap,
                                                         const char **argv_out, size_t argv_cap,
                                                         size_t *arg_len_out)
{
   if (!in || len < 8 || !name_out || name_cap == 0 || !argv_out || !arg_len_out ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_LAUNCH_RESPONSE_MAGIC)
      return -1;
   size_t name_len = aimee_delegates_get_u32(in + 4);
   if (8 + name_len + 4 > len || name_len >= name_cap)
      return -1;
   memcpy(name_out, in + 8, name_len);
   name_out[name_len] = '\0';

   size_t at = 8 + name_len;
   size_t argc = aimee_delegates_get_u32(in + at);
   at += 4;
   if (argc > argv_cap)
      return -1;
   for (size_t i = 0; i < argc; i++)
   {
      if (at + 4 > len)
         return -1;
      size_t n = aimee_delegates_get_u32(in + at);
      at += 4;
      if (at + n > len)
         return -1;
      argv_out[i] = (const char *)(in + at);
      arg_len_out[i] = n;
      at += n;
   }
   if (at != len)
      return -1;
   return (int)argc;
}

/* --- Image spec (stage 13): the Dockerfile a sandbox is built from, and the
 * tag that names it ---
 *
 * Both together, because they are one decision: the tag is a hash OF the text.
 * Computing them apart is how an image gets built under a name describing
 * different content, which defeats the reuse the content tag exists for. */

#define AIMEE_DELEGATES_EVENT_IMGSPEC          6669u
#define AIMEE_DELEGATES_STAGE_IMGSPEC          13u
#define AIMEE_DELEGATES_IMGSPEC_REQUEST_MAGIC  0x51494d44u /* "DMIQ" */
#define AIMEE_DELEGATES_IMGSPEC_RESPONSE_MAGIC 0x53494d44u /* "DMIS" */
#define AIMEE_DELEGATES_IMGSPEC_HEADER_LEN     16u
#define AIMEE_DELEGATES_IMGSPEC_MAX_PACKAGES   512u

/* Returns the encoded length, or 0 when it does not fit. */
static inline size_t aimee_delegates_imgspec_request_encode(const char *base,
                                                            const char *const *pkgs, int npkgs,
                                                            const char *verbatim, uint8_t *out,
                                                            size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_IMGSPEC_HEADER_LEN || npkgs < 0 ||
       (unsigned)npkgs > AIMEE_DELEGATES_IMGSPEC_MAX_PACKAGES)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_IMGSPEC_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_IMGSPEC_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   aimee_delegates_put_u32(out + 8, (uint32_t)npkgs);

   size_t at = aimee_delegates_launch_field(out, cap, AIMEE_DELEGATES_IMGSPEC_HEADER_LEN, base);
   if (at == 0)
      return 0;
   for (int i = 0; i < npkgs; i++)
   {
      at = aimee_delegates_launch_field(out, cap, at, pkgs ? pkgs[i] : NULL);
      if (at == 0)
         return 0;
   }
   /* An operator-committed Dockerfile, carried whole. Mutually exclusive with
    * base+packages: the module refuses a request that supplies both. */
   return aimee_delegates_launch_field(out, cap, at, verbatim);
}

/* Both outputs are NUL-terminated. Returns 0, or -1 (including when either
 * would be truncated -- a truncated Dockerfile builds a DIFFERENT image than
 * the tag names, and a truncated tag collides two images onto one name). */
static inline int aimee_delegates_imgspec_response_decode(const uint8_t *in, size_t len, char *tag,
                                                          size_t tag_cap, char *dockerfile,
                                                          size_t df_cap)
{
   if (!in || len < 12 || !tag || !tag_cap || !dockerfile || !df_cap ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_IMGSPEC_RESPONSE_MAGIC)
      return -1;
   size_t tag_len = aimee_delegates_get_u32(in + 4);
   size_t df_len = aimee_delegates_get_u32(in + 8);
   if (12 + tag_len + df_len != len || tag_len >= tag_cap || df_len >= df_cap)
      return -1;
   memcpy(tag, in + 12, tag_len);
   tag[tag_len] = '\0';
   memcpy(dockerfile, in + 12 + tag_len, df_len);
   dockerfile[df_len] = '\0';
   return 0;
}

/* --- Isolation verdict (stage 14): what a container's network report means ---
 *
 * The caller runs the probe; the module reads it and judges. Reading and
 * judging are one rule because the whole difficulty is that "the probe failed"
 * and "the sandbox is open" look identical from the caller's side. */

#define AIMEE_DELEGATES_EVENT_ISOLATION          6670u
#define AIMEE_DELEGATES_STAGE_ISOLATION          14u
#define AIMEE_DELEGATES_ISOLATION_REQUEST_MAGIC  0x51534944u /* "DISQ" */
#define AIMEE_DELEGATES_ISOLATION_RESPONSE_MAGIC 0x53534944u /* "DISS" */
#define AIMEE_DELEGATES_ISOLATION_HEADER_LEN     16u
#define AIMEE_DELEGATES_ISOLATION_PROBE_FAILED   1u
#define AIMEE_DELEGATES_ISOLATION_REQUIRE        2u
#define AIMEE_DELEGATES_ISOLATION_REPORT_MAX     (1u << 16)

static inline size_t aimee_delegates_isolation_request_encode(const char *report, int probe_failed,
                                                              int require_isolation, uint8_t *out,
                                                              size_t cap)
{
   size_t len = report ? strlen(report) : 0;
   if (!out || cap < AIMEE_DELEGATES_ISOLATION_HEADER_LEN + len ||
       len > AIMEE_DELEGATES_ISOLATION_REPORT_MAX)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_ISOLATION_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_ISOLATION_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = (uint8_t)((probe_failed ? AIMEE_DELEGATES_ISOLATION_PROBE_FAILED : 0u) |
                      (require_isolation ? AIMEE_DELEGATES_ISOLATION_REQUIRE : 0u));
   aimee_delegates_put_u32(out + 8, (uint32_t)len);
   if (len)
      memcpy(out + AIMEE_DELEGATES_ISOLATION_HEADER_LEN, report, len);
   return AIMEE_DELEGATES_ISOLATION_HEADER_LEN + len;
}

/* `reason` receives the operator-facing wording, NUL-terminated. It travels
 * with the verdict rather than being written here: a caller that phrased its
 * own would describe a judgement it did not make. */
static inline int aimee_delegates_isolation_response_decode(const uint8_t *in, size_t len,
                                                            int *refuse, int *warn, int *is_error,
                                                            char *reason, size_t reason_cap)
{
   if (!in || len < 16 || !refuse || !warn || !is_error ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_ISOLATION_RESPONSE_MAGIC)
      return -1;
   *refuse = aimee_delegates_get_u32(in + 4) ? 1 : 0;
   *warn = aimee_delegates_get_u32(in + 8) ? 1 : 0;
   /* Severity, not inferred from the wording: a breach that runs anyway is an
    * error, a flaky probe is not. */
   *is_error = aimee_delegates_get_u32(in + 12) ? 1 : 0;
   if (reason && reason_cap)
   {
      size_t n = len - 16;
      if (n >= reason_cap)
         n = reason_cap - 1;
      memcpy(reason, in + 16, n);
      reason[n] = '\0';
   }
   return 0;
}

/* --- May write (stage 15): the role AND the brief, composed ---------------
 *
 * One answer, because it is the one fact stages 11 and 12 must agree on. The
 * halves come back too, so a refusal is debuggable: "the delegate could not
 * edit anything" is otherwise a mystery. */

#define AIMEE_DELEGATES_EVENT_MAYWRITE          6671u
#define AIMEE_DELEGATES_STAGE_MAYWRITE          15u
#define AIMEE_DELEGATES_MAYWRITE_REQUEST_MAGIC  0x51575744u /* "DWWQ" */
#define AIMEE_DELEGATES_MAYWRITE_RESPONSE_MAGIC 0x53575744u /* "DWWS" */
#define AIMEE_DELEGATES_MAYWRITE_HEADER_LEN     16u
#define AIMEE_DELEGATES_MAYWRITE_RESPONSE_LEN   16u
#define AIMEE_DELEGATES_MAYWRITE_PROMPT_MAX     (1u << 20)

/* Returns the encoded length, or 0 when it does not fit. */
static inline size_t aimee_delegates_maywrite_request_encode(const char *role, const char *prompt,
                                                             uint8_t *out, size_t cap)
{
   size_t role_len = role ? strlen(role) : 0;
   size_t prompt_len = prompt ? strlen(prompt) : 0;
   size_t total = AIMEE_DELEGATES_MAYWRITE_HEADER_LEN + role_len + prompt_len;
   if (!out || cap < total || role_len > AIMEE_DELEGATES_ROLE_MAX ||
       prompt_len > AIMEE_DELEGATES_MAYWRITE_PROMPT_MAX)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_MAYWRITE_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_MAYWRITE_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   aimee_delegates_put_u32(out + 8, (uint32_t)role_len);
   aimee_delegates_put_u32(out + 12, (uint32_t)prompt_len);
   if (role_len)
      memcpy(out + AIMEE_DELEGATES_MAYWRITE_HEADER_LEN, role, role_len);
   if (prompt_len)
      memcpy(out + AIMEE_DELEGATES_MAYWRITE_HEADER_LEN + role_len, prompt, prompt_len);
   return total;
}

/* `by_role` and `by_prompt` are optional and are for reporting only -- the
 * decision is `may_write`. */
static inline int aimee_delegates_maywrite_response_decode(const uint8_t *in, size_t len,
                                                           int *may_write, int *by_role,
                                                           int *by_prompt)
{
   if (!in || len != AIMEE_DELEGATES_MAYWRITE_RESPONSE_LEN || !may_write ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_MAYWRITE_RESPONSE_MAGIC)
      return -1;
   *may_write = aimee_delegates_get_u32(in + 4) ? 1 : 0;
   if (by_role)
      *by_role = aimee_delegates_get_u32(in + 8) ? 1 : 0;
   if (by_prompt)
      *by_prompt = aimee_delegates_get_u32(in + 12) ? 1 : 0;
   return 0;
}

/* --- Image GC (stage 16): which built sandbox images may be deleted --------
 *
 * The whole inventory goes at once. The decision is POSITIONAL -- "keep the
 * keep_min most recent" cannot be answered one image at a time -- so the
 * ordering belongs with the rule rather than with the caller. */

#define AIMEE_DELEGATES_EVENT_IMGGC          6672u
#define AIMEE_DELEGATES_STAGE_IMGGC          16u
#define AIMEE_DELEGATES_IMGGC_REQUEST_MAGIC  0x51434744u /* "DGCQ" */
#define AIMEE_DELEGATES_IMGGC_RESPONSE_MAGIC 0x53434744u /* "DGCS" */
#define AIMEE_DELEGATES_IMGGC_HEADER_LEN     32u
#define AIMEE_DELEGATES_IMGGC_MAX_IMAGES     4096u

static inline void aimee_delegates_put_u64(uint8_t *p, uint64_t v)
{
   for (unsigned i = 0; i < 8; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

/* Begin a request. Returns the offset to append images at, or 0 on failure. */
static inline size_t aimee_delegates_imggc_request_begin(unsigned count, int keep_min,
                                                         long long now, long long max_age_secs,
                                                         uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_IMGGC_HEADER_LEN ||
       count > AIMEE_DELEGATES_IMGGC_MAX_IMAGES)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_IMGGC_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_IMGGC_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   aimee_delegates_put_u32(out + 8, count);
   aimee_delegates_put_u32(out + 12, (uint32_t)keep_min);
   aimee_delegates_put_u64(out + 16, (uint64_t)now);
   aimee_delegates_put_u64(out + 24, (uint64_t)max_age_secs);
   return AIMEE_DELEGATES_IMGGC_HEADER_LEN;
}

/* Append one image. Returns the new offset, or 0 when it does not fit. */
static inline size_t aimee_delegates_imggc_request_add(uint8_t *out, size_t cap, size_t at,
                                                       const char *tag, const char *created_at,
                                                       int in_use)
{
   size_t tag_len = tag ? strlen(tag) : 0;
   size_t created_len = created_at ? strlen(created_at) : 0;
   if (at + 12 + tag_len + created_len > cap)
      return 0;
   aimee_delegates_put_u32(out + at, in_use ? 1u : 0u);
   aimee_delegates_put_u32(out + at + 4, (uint32_t)tag_len);
   aimee_delegates_put_u32(out + at + 8, (uint32_t)created_len);
   at += 12;
   if (tag_len)
      memcpy(out + at, tag, tag_len);
   at += tag_len;
   if (created_len)
      memcpy(out + at, created_at, created_len);
   return at + created_len;
}

/* Read the verdict for image `index`, in the order they were sent. `reason`
 * receives a NUL-terminated word. Returns 0, or -1. */
static inline int aimee_delegates_imggc_response_at(const uint8_t *in, size_t len, unsigned index,
                                                    int *remove_out, char *reason,
                                                    size_t reason_cap)
{
   if (!in || len < 8 || !remove_out ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_IMGGC_RESPONSE_MAGIC)
      return -1;
   unsigned count = aimee_delegates_get_u32(in + 4);
   if (index >= count)
      return -1;
   size_t at = 8;
   for (unsigned i = 0; i <= index; i++)
   {
      if (at + 8 > len)
         return -1;
      unsigned rm = aimee_delegates_get_u32(in + at);
      size_t n = aimee_delegates_get_u32(in + at + 4);
      at += 8;
      if (at + n > len)
         return -1;
      if (i == index)
      {
         *remove_out = rm ? 1 : 0;
         if (reason && reason_cap)
         {
            size_t copy = n < reason_cap - 1 ? n : reason_cap - 1;
            memcpy(reason, in + at, copy);
            reason[copy] = '\0';
         }
         return 0;
      }
      at += n;
   }
   return -1;
}

/* --- Route filter (stage 17): which agents may serve this packet -----------
 *
 * The whole fleet at once. The modality relaxation is a decision ABOUT the
 * fleet -- it fires only when requiring modality would leave nobody and
 * dropping it would leave somebody -- so it cannot be asked per agent. */

#define AIMEE_DELEGATES_EVENT_ROUTEFILTER          6673u
#define AIMEE_DELEGATES_STAGE_ROUTEFILTER          17u
#define AIMEE_DELEGATES_ROUTEFILTER_REQUEST_MAGIC  0x51525444u /* "DTRQ" */
#define AIMEE_DELEGATES_ROUTEFILTER_RESPONSE_MAGIC 0x53525444u /* "DTRS" */
#define AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN     24u
#define AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN      32u
#define AIMEE_DELEGATES_ROUTEFILTER_MAX_AGENTS     4096u

#define AIMEE_DELEGATES_RF_ENABLED    1u
#define AIMEE_DELEGATES_RF_HAS_ROLE   2u
#define AIMEE_DELEGATES_RF_HAVE_CAP   4u
#define AIMEE_DELEGATES_RF_DEPRECATED 8u
#define AIMEE_DELEGATES_RF_TOOLS      16u

static inline size_t aimee_delegates_routefilter_request_begin(unsigned count,
                                                               unsigned required_caps,
                                                               int min_context, int drop_deprecated,
                                                               uint8_t *out, size_t cap)
{
   if (!out || count > AIMEE_DELEGATES_ROUTEFILTER_MAX_AGENTS ||
       cap < AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN +
                 (size_t)count * AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN)
      return 0;
   memset(out, 0, AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_ROUTEFILTER_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   out[5] = drop_deprecated ? 1u : 0u;
   aimee_delegates_put_u32(out + 8, count);
   aimee_delegates_put_u32(out + 12, required_caps);
   aimee_delegates_put_u32(out + 16, (uint32_t)min_context);
   return AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN;
}

/* Every agent slot is fixed-width, so `index` addresses it directly. */
static inline void aimee_delegates_routefilter_request_set(uint8_t *out, unsigned index,
                                                           unsigned flags, unsigned cap_flags,
                                                           int override_ctx, int catalog_ctx,
                                                           int cli_ctx)
{
   uint8_t *at = out + AIMEE_DELEGATES_ROUTEFILTER_HEADER_LEN +
                 (size_t)index * AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN;
   memset(at, 0, AIMEE_DELEGATES_ROUTEFILTER_AGENT_LEN);
   aimee_delegates_put_u32(at, flags);
   aimee_delegates_put_u32(at + 4, cap_flags);
   aimee_delegates_put_u32(at + 8, (uint32_t)override_ctx);
   aimee_delegates_put_u32(at + 12, (uint32_t)catalog_ctx);
   aimee_delegates_put_u32(at + 16, (uint32_t)cli_ctx);
}

/* `keep` receives one int per agent, in the order they were sent. */
static inline int aimee_delegates_routefilter_response_decode(const uint8_t *in, size_t len,
                                                              unsigned count, int *kept_out,
                                                              int *relaxed_out,
                                                              unsigned *effective_caps_out,
                                                              int *keep, size_t keep_cap)
{
   if (!in || len != 16u + (size_t)count * 4u || !keep || keep_cap < count ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_ROUTEFILTER_RESPONSE_MAGIC)
      return -1;
   if (kept_out)
      *kept_out = (int)aimee_delegates_get_u32(in + 4);
   if (relaxed_out)
      *relaxed_out = aimee_delegates_get_u32(in + 8) ? 1 : 0;
   if (effective_caps_out)
      *effective_caps_out = aimee_delegates_get_u32(in + 12);
   for (unsigned i = 0; i < count; i++)
      keep[i] = aimee_delegates_get_u32(in + 16 + i * 4) ? 1 : 0;
   return 0;
}

/* --- No-op write (stage 18): did a successful write delegate change anything?
 *
 * The caller gathers the evidence -- file snapshots, `git status`, a HEAD
 * comparison -- because all three are I/O. Only the reading of it is the
 * module's. */

#define AIMEE_DELEGATES_EVENT_NOOPWRITE          6674u
#define AIMEE_DELEGATES_STAGE_NOOPWRITE          18u
#define AIMEE_DELEGATES_NOOPWRITE_REQUEST_MAGIC  0x51574e44u /* "DNWQ" */
#define AIMEE_DELEGATES_NOOPWRITE_RESPONSE_MAGIC 0x53574e44u /* "DNWS" */
#define AIMEE_DELEGATES_NOOPWRITE_REQUEST_LEN    16u

/* Bit 0 was IS_WRITE_ROLE, which said the same thing as WRITES_ALLOWED. */
#define AIMEE_DELEGATES_NOOP_WRITES_ALLOWED (1u << 1)
#define AIMEE_DELEGATES_NOOP_HANDOFF_JSON   (1u << 2)
#define AIMEE_DELEGATES_NOOP_SUCCEEDED      (1u << 3)
#define AIMEE_DELEGATES_NOOP_ANY_NAMED      (1u << 4)
#define AIMEE_DELEGATES_NOOP_WORKTREE_DIRTY (1u << 5)
#define AIMEE_DELEGATES_NOOP_HEAD_ADVANCED  (1u << 6)
#define AIMEE_DELEGATES_NOOP_HEAD_SNAPSHOT  (1u << 7)
#define AIMEE_DELEGATES_NOOP_HAS_WORKTREE   (1u << 8)

static inline int aimee_delegates_noopwrite_request_encode(unsigned flags, int named_count,
                                                           uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_DELEGATES_NOOPWRITE_REQUEST_LEN || named_count < 0)
      return -1;
   memset(out, 0, AIMEE_DELEGATES_NOOPWRITE_REQUEST_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_NOOPWRITE_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   aimee_delegates_put_u32(out + 8, flags);
   aimee_delegates_put_u32(out + 12, (uint32_t)named_count);
   return 0;
}

/* `message` receives the operator-facing wording, NUL-terminated. It is present
 * for the BENIGN cases too: nothing failed there, so what it says is the only
 * evidence the guard ran at all. */
static inline int aimee_delegates_noopwrite_response_decode(const uint8_t *in, size_t len,
                                                            int *noop, int *benign, char *message,
                                                            size_t message_cap)
{
   if (!in || len < 12 || !noop || !benign ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_NOOPWRITE_RESPONSE_MAGIC)
      return -1;
   *noop = aimee_delegates_get_u32(in + 4) ? 1 : 0;
   *benign = aimee_delegates_get_u32(in + 8) ? 1 : 0;
   if (message && message_cap)
   {
      size_t n = len - 12;
      if (n >= message_cap)
         n = message_cap - 1;
      memcpy(message, in + 12, n);
      message[n] = '\0';
   }
   return 0;
}

/* --- Launch plan (stage 19): what a delegate plan becomes.
 *
 * Every other stage here is fixed-width. This one carries prose -- titles,
 * objectives, paths, and the briefs it returns -- so it is length-prefixed:
 * each string is a u32 length followed by that many bytes, each list a u32
 * count followed by its elements.
 *
 * The caller supplies two facts per owned file because the module does no I/O:
 * whether the file exists, and (only when it does not) the tracked files that
 * share its basename. Candidates rather than the whole file list, because the
 * whole list does not fit: this repository's is 208KB against a 1MiB payload.
 *
 * The module returns rows; the caller writes them. Creating the execution plan,
 * the coord job, and the task rows stays with the caller, which owns the
 * database. */

#define AIMEE_DELEGATES_EVENT_LAUNCHPLAN          6675u
#define AIMEE_DELEGATES_STAGE_LAUNCHPLAN          19u
#define AIMEE_DELEGATES_LAUNCHPLAN_REQUEST_MAGIC  0x514c5044u /* "DPLQ" */
#define AIMEE_DELEGATES_LAUNCHPLAN_RESPONSE_MAGIC 0x534c5044u /* "DPLS" */

/* A bounded append cursor. Once it overflows it stays overflowed, so a caller
 * may build the whole request and check once at the end rather than after every
 * field -- a check that is easy to forget on one field out of twenty. */
typedef struct
{
   uint8_t *buf;
   size_t cap;
   size_t len;
   int overflow;
} aimee_delegates_wire_t;

static inline void aimee_delegates_wire_init(aimee_delegates_wire_t *w, uint8_t *buf, size_t cap)
{
   w->buf = buf;
   w->cap = cap;
   w->len = 0;
   w->overflow = 0;
}

static inline void aimee_delegates_wire_u32(aimee_delegates_wire_t *w, uint32_t v)
{
   if (w->overflow || w->len + 4 > w->cap)
   {
      w->overflow = 1;
      return;
   }
   aimee_delegates_put_u32(w->buf + w->len, v);
   w->len += 4;
}

static inline void aimee_delegates_wire_str(aimee_delegates_wire_t *w, const char *s)
{
   size_t n = s ? strlen(s) : 0;
   aimee_delegates_wire_u32(w, (uint32_t)n);
   if (w->overflow || w->len + n > w->cap)
   {
      w->overflow = 1;
      return;
   }
   if (n)
      memcpy(w->buf + w->len, s, n);
   w->len += n;
}

/* Begin a request. Follow with the plan fields in wire order:
 *   missing_owned_files: count, then each string
 *   packets:             count, then per packet id, title, objective, role,
 *                        handoff_schema, owned-file count, and per file its
 *                        path, an exists flag, and its candidate list. */
static inline void aimee_delegates_launchplan_request_begin(aimee_delegates_wire_t *w, uint8_t *buf,
                                                            size_t cap, int max_concurrent,
                                                            const char *schema, const char *title)
{
   aimee_delegates_wire_init(w, buf, cap);
   aimee_delegates_wire_u32(w, AIMEE_DELEGATES_LAUNCHPLAN_REQUEST_MAGIC);
   aimee_delegates_wire_u32(w, (uint32_t)AIMEE_DELEGATES_WIRE_VERSION);
   aimee_delegates_wire_u32(w, (uint32_t)max_concurrent);
   aimee_delegates_wire_str(w, schema);
   aimee_delegates_wire_str(w, title);
}

/* A read cursor over the response, which refuses to read past its end. */
typedef struct
{
   const uint8_t *buf;
   size_t len;
   size_t at;
   int bad;
} aimee_delegates_rd_t;

static inline uint32_t aimee_delegates_rd_u32(aimee_delegates_rd_t *r)
{
   if (r->bad || r->at + 4 > r->len)
   {
      r->bad = 1;
      return 0;
   }
   uint32_t v = aimee_delegates_get_u32(r->buf + r->at);
   r->at += 4;
   return v;
}

/* Copies the next string into `out` (always NUL-terminated when out_cap > 0).
 * A string too long for `out` is TRUNCATED, not an error: the cursor still
 * advances past all of it, so one oversized field cannot desynchronise the
 * rest of the response. */
static inline void aimee_delegates_rd_str(aimee_delegates_rd_t *r, char *out, size_t out_cap)
{
   uint32_t n = aimee_delegates_rd_u32(r);
   if (r->bad || r->at + (size_t)n > r->len)
   {
      r->bad = 1;
      if (out && out_cap)
         out[0] = '\0';
      return;
   }
   if (out && out_cap)
   {
      size_t copy = (size_t)n < out_cap - 1 ? (size_t)n : out_cap - 1;
      memcpy(out, r->buf + r->at, copy);
      out[copy] = '\0';
   }
   r->at += n;
}

/* Reads the response header: the error string (empty when the plan may launch)
 * and the effective concurrency. Leaves `r` positioned at the step count. */
static inline int aimee_delegates_launchplan_response_begin(aimee_delegates_rd_t *r,
                                                            const uint8_t *in, size_t len,
                                                            char *errbuf, size_t errbuf_cap,
                                                            int *max_concurrent)
{
   r->buf = in;
   r->len = len;
   r->at = 0;
   r->bad = 0;
   if (!in || aimee_delegates_rd_u32(r) != AIMEE_DELEGATES_LAUNCHPLAN_RESPONSE_MAGIC)
      return -1;
   aimee_delegates_rd_str(r, errbuf, errbuf_cap);
   uint32_t par = aimee_delegates_rd_u32(r);
   if (max_concurrent)
      *max_concurrent = (int)par;
   return r->bad ? -1 : 0;
}

/* --- Review evidence (stage 20): did a review look at what it reviewed?
 *
 * The caller supplies the two facts the module cannot compute -- whether the
 * review target arrived IN the prompt, and whether the worktree is dirty -- and
 * gets back what to check and what is already wrong.
 *
 * The citation-vs-checkout comparison stays with the caller because it reads
 * the checkout. What the module decides is which roles warrant it. */

#define AIMEE_DELEGATES_EVENT_REVIEWEV          6676u
#define AIMEE_DELEGATES_STAGE_REVIEWEV          20u
#define AIMEE_DELEGATES_REVIEWEV_REQUEST_MAGIC  0x51455244u /* "DREQ" */
#define AIMEE_DELEGATES_REVIEWEV_RESPONSE_MAGIC 0x53455244u /* "DRES" */
#define AIMEE_DELEGATES_REVIEWEV_HEADER_LEN     16u

#define AIMEE_DELEGATES_REVIEW_TARGET_PROVIDED (1u << 0)
#define AIMEE_DELEGATES_REVIEW_WORKTREE_DIRTY  (1u << 1)

#define AIMEE_DELEGATES_REVIEW_GUARDED        (1u << 0)
#define AIMEE_DELEGATES_REVIEW_CHECK_SNIPPETS (1u << 1)
#define AIMEE_DELEGATES_REVIEW_CONTRADICTION  (1u << 2)

#define AIMEE_DELEGATES_REVIEW_ROLE_MAX 64u

/* Returns the encoded length, or -1 when the buffer is too small (in which case
 * nothing is written -- a truncated response would be judged on a partial
 * report, which is exactly the failure this stage exists to catch). */
static inline int aimee_delegates_reviewev_request_encode(const char *role, const char *response,
                                                          unsigned flags, uint8_t *out, size_t cap)
{
   size_t role_len = role ? strlen(role) : 0;
   size_t response_len = response ? strlen(response) : 0;
   if (!out || role_len > AIMEE_DELEGATES_REVIEW_ROLE_MAX)
      return -1;
   size_t need = AIMEE_DELEGATES_REVIEWEV_HEADER_LEN + role_len + response_len;
   if (need > cap)
      return -1;

   memset(out, 0, AIMEE_DELEGATES_REVIEWEV_HEADER_LEN);
   aimee_delegates_put_u32(out, AIMEE_DELEGATES_REVIEWEV_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_DELEGATES_WIRE_VERSION;
   aimee_delegates_put_u32(out + 8, flags);
   aimee_delegates_put_u32(out + 12, (uint32_t)role_len);
   if (role_len)
      memcpy(out + AIMEE_DELEGATES_REVIEWEV_HEADER_LEN, role, role_len);
   if (response_len)
      memcpy(out + AIMEE_DELEGATES_REVIEWEV_HEADER_LEN + role_len, response, response_len);
   return (int)need;
}

/* `message` receives the contradiction wording, NUL-terminated and empty when
 * there is none. */
static inline int aimee_delegates_reviewev_response_decode(const uint8_t *in, size_t len,
                                                           unsigned *verdict, char *message,
                                                           size_t message_cap)
{
   if (!in || len < 8 || !verdict ||
       aimee_delegates_get_u32(in) != AIMEE_DELEGATES_REVIEWEV_RESPONSE_MAGIC)
      return -1;
   *verdict = aimee_delegates_get_u32(in + 4);
   if (message && message_cap)
   {
      size_t n = len - 8;
      if (n >= message_cap)
         n = message_cap - 1;
      memcpy(message, in + 8, n);
      message[n] = '\0';
   }
   return 0;
}

/* --- Named-file drift (stage 21): did the delegate touch the files its brief
 * named?
 *
 * Length-prefixed, like the launch-plan stage: prose plus a variable number of
 * paths. Build a request with _begin, then one _path per named file.
 *
 * The caller supplies, per path, only facts: does it exist, does `git diff`
 * mention it, and which files the code index returned for its basename stem.
 * An EMPTY hit list is meaningful -- it says the index was unreachable or the
 * stem is unindexed, which is not the same as the index saying the path is not
 * ours -- so send it empty rather than skipping the path. */

#define AIMEE_DELEGATES_EVENT_DRIFT          6677u
#define AIMEE_DELEGATES_STAGE_DRIFT          21u
#define AIMEE_DELEGATES_DRIFT_REQUEST_MAGIC  0x51465244u /* "DRFQ" */
#define AIMEE_DELEGATES_DRIFT_RESPONSE_MAGIC 0x53465244u /* "DRFS" */

#define AIMEE_DELEGATES_DRIFT_WRITES_ALLOWED (1u << 0)

#define AIMEE_DELEGATES_DRIFT_PATH_EXISTS  (1u << 0)
#define AIMEE_DELEGATES_DRIFT_PATH_IN_DIFF (1u << 1)

/* Severities, matching the module's DriftSeverity. */
#define AIMEE_DELEGATES_DRIFT_NONE 0u
#define AIMEE_DELEGATES_DRIFT_SOFT 1u
#define AIMEE_DELEGATES_DRIFT_HARD 2u

static inline void aimee_delegates_drift_request_begin(aimee_delegates_wire_t *w, uint8_t *buf,
                                                       size_t cap, unsigned flags,
                                                       const char *prompt, const char *response,
                                                       const char *worktree_path)
{
   aimee_delegates_wire_init(w, buf, cap);
   aimee_delegates_wire_u32(w, AIMEE_DELEGATES_DRIFT_REQUEST_MAGIC);
   aimee_delegates_wire_u32(w, (uint32_t)AIMEE_DELEGATES_WIRE_VERSION);
   aimee_delegates_wire_u32(w, flags);
   aimee_delegates_wire_str(w, prompt);
   aimee_delegates_wire_str(w, response);
   aimee_delegates_wire_str(w, worktree_path);
}

/* Append one named path. `hits` may be NULL when hit_count is 0. */
static inline void aimee_delegates_drift_request_path(aimee_delegates_wire_t *w, const char *path,
                                                      unsigned path_flags,
                                                      const char *const *hits, int hit_count)
{
   aimee_delegates_wire_str(w, path);
   aimee_delegates_wire_u32(w, path_flags);
   aimee_delegates_wire_u32(w, (uint32_t)(hit_count > 0 ? hit_count : 0));
   for (int i = 0; i < hit_count; i++)
      aimee_delegates_wire_str(w, hits[i]);
}

static inline int aimee_delegates_drift_response_decode(const uint8_t *in, size_t len,
                                                        unsigned *severity, char *message,
                                                        size_t message_cap)
{
   aimee_delegates_rd_t r;
   r.buf = in;
   r.len = len;
   r.at = 0;
   r.bad = 0;
   if (!in || !severity ||
       aimee_delegates_rd_u32(&r) != AIMEE_DELEGATES_DRIFT_RESPONSE_MAGIC)
      return -1;
   *severity = aimee_delegates_rd_u32(&r);
   aimee_delegates_rd_str(&r, message, message_cap);
   return r.bad ? -1 : 0;
}

/* --- Delegate permissions (stage 15): what a delegate may do.
 *
 * Resolved ONCE, when the delegate is created, and carried for the life of the
 * run. Nothing downstream works the answer out again: the mount reads it, the
 * tool allowlist reads it, the API reads it. A permission derived twice can
 * disagree with itself, which this codebase has done twice.
 *
 * The caller sends the role, and the role definition when an operator wrote one.
 * What comes back is the resolved set: each permission, where it is enforced,
 * what it is scoped to, and the list of permissions nothing is enforcing.
 *
 * See docs/DELEGATE_ROLE_PERMISSIONS.md. */

#define AIMEE_DELEGATES_EVENT_PERMS          6671u
#define AIMEE_DELEGATES_STAGE_PERMS          15u
#define AIMEE_DELEGATES_PERMS_REQUEST_MAGIC  0x51524550u /* "PERQ" */
#define AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC 0x53524550u /* "PERS" */

/* A role definition follows. Absent, the built-in table answers, and the two
 * differ: a definition granting nothing is a deliberate powerless role, while no
 * definition at all falls back to what ships. */
#define AIMEE_DELEGATES_PERMS_DEFINED (1u << 0)

/* The permissions this build guarantees, and where each is enforced. Names are
 * the contract with whoever writes a role definition. */
#define AIMEE_DELEGATES_PERM_TOOLS           "tools"
#define AIMEE_DELEGATES_PERM_SHELL           "shell"
#define AIMEE_DELEGATES_PERM_REPO_WRITE      "repo_write"
#define AIMEE_DELEGATES_PERM_KNOWLEDGE_WRITE "knowledge_write"

static inline void aimee_delegates_perms_request_begin(aimee_delegates_wire_t *w, uint8_t *buf,
                                                       size_t cap, unsigned flags, const char *role)
{
   aimee_delegates_wire_init(w, buf, cap);
   aimee_delegates_wire_u32(w, AIMEE_DELEGATES_PERMS_REQUEST_MAGIC);
   aimee_delegates_wire_u32(w, (uint32_t)AIMEE_DELEGATES_WIRE_VERSION);
   aimee_delegates_wire_u32(w, flags);
   aimee_delegates_wire_str(w, role);
}

/* Append the role template frontmatter an operator wrote, verbatim.
 *
 * Only when AIMEE_DELEGATES_PERMS_DEFINED is set. The module parses it: what a
 * permission block MEANS is a rule, and this side's job is to hand over the
 * bytes it found on disk. A block the module cannot read fails the whole
 * request rather than falling back to the built-in table -- falling back would
 * hand a delegate the powers its role ships with while the operator believes it
 * holds the ones they wrote. */
static inline void aimee_delegates_perms_request_definition(aimee_delegates_wire_t *w,
                                                            const char *frontmatter)
{
   aimee_delegates_wire_str(w, frontmatter ? frontmatter : "");
}

/* Reads the response header and leaves `r` positioned at the first grant.
 * Returns the grant count, or -1 on a malformed response. */
static inline int aimee_delegates_perms_response_begin(aimee_delegates_rd_t *r, const uint8_t *in,
                                                       size_t len)
{
   r->buf = in;
   r->len = len;
   r->at = 0;
   r->bad = 0;
   if (!in || aimee_delegates_rd_u32(r) != AIMEE_DELEGATES_PERMS_RESPONSE_MAGIC)
      return -1;
   uint32_t count = aimee_delegates_rd_u32(r);
   return r->bad ? -1 : (int)count;
}

/* Reads one grant: its name, where it is enforced, and how many scopes follow.
 * The scopes themselves are read with aimee_delegates_rd_str. */
static inline int aimee_delegates_perms_response_grant(aimee_delegates_rd_t *r, char *name,
                                                       size_t name_cap, char *enforced_at,
                                                       size_t enforced_cap, int *scope_count)
{
   aimee_delegates_rd_str(r, name, name_cap);
   aimee_delegates_rd_str(r, enforced_at, enforced_cap);
   uint32_t count = aimee_delegates_rd_u32(r);
   if (r->bad)
      return -1;
   if (scope_count)
      *scope_count = (int)count;
   return 0;
}

#endif
