/* Wire contract for workspace project-reference admission. */
#ifndef AIMEE_WORKSPACE_MODULE_API_H
#define AIMEE_WORKSPACE_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_WORKSPACE_EVENT_ACCESS   7169u
#define AIMEE_WORKSPACE_STAGE_ACCESS   1u
#define AIMEE_WORKSPACE_REQUEST_MAGIC  0x46455257u /* "WREF" */
#define AIMEE_WORKSPACE_RESPONSE_MAGIC 0x4b4f5757u /* "WWOK" */
#define AIMEE_WORKSPACE_WIRE_VERSION   1u
#define AIMEE_WORKSPACE_REF_MAX        129u
#define AIMEE_WORKSPACE_REQUEST_LEN    140u
#define AIMEE_WORKSPACE_RESPONSE_LEN   8u

static inline void aimee_workspace_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_workspace_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline void aimee_workspace_put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8u);
}

static inline uint16_t aimee_workspace_get_u16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static inline int aimee_workspace_request_encode(const char *ref, size_t ref_len, uint8_t *out,
                                                 size_t cap)
{
   if (!out || cap < AIMEE_WORKSPACE_REQUEST_LEN || !ref || ref_len == 0 ||
       ref_len > AIMEE_WORKSPACE_REF_MAX)
      return -1;
   memset(out, 0, AIMEE_WORKSPACE_REQUEST_LEN);
   aimee_workspace_put_u32(out, AIMEE_WORKSPACE_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_WORKSPACE_WIRE_VERSION;
   aimee_workspace_put_u16(out + 6, (uint16_t)ref_len);
   memcpy(out + 8, ref, ref_len);
   return 0;
}

/* Runner stages. The module owns which client is serving which tree and the
 * handoff of work to it; these only frame the question. Kinds are one per stage
 * because the bus maps a kind to exactly one stage. */
#define AIMEE_WORKSPACE_EVENT_RUNNER    7170u
#define AIMEE_WORKSPACE_STAGE_RUNNER    2u
#define AIMEE_WORKSPACE_EVENT_RUNNER_IO 7171u
#define AIMEE_WORKSPACE_STAGE_RUNNER_IO 3u

#define AIMEE_WS_RUNNER_REQUEST_MAGIC  0x4e555257u /* "WRUN" */
#define AIMEE_WS_RUNNER_RESPONSE_MAGIC 0x56535257u /* "WRSV" */
#define AIMEE_WS_RUNNER_ID_MAX         128u
#define AIMEE_WS_RUNNER_PATH_MAX       1024u
#define AIMEE_WS_RUNNER_REQUEST_LEN    (8u + AIMEE_WS_RUNNER_PATH_MAX)
#define AIMEE_WS_RUNNER_RESPONSE_LEN   (8u + AIMEE_WS_RUNNER_ID_MAX)

#define AIMEE_WS_RUNNER_OP_REGISTER   1u
#define AIMEE_WS_RUNNER_OP_UNREGISTER 2u
#define AIMEE_WS_RUNNER_OP_RESOLVE    3u

#define AIMEE_WS_IO_REQUEST_MAGIC   0x4f495257u /* "WRIO" */
#define AIMEE_WS_IO_RESPONSE_MAGIC  0x52495257u /* "WRIR" */
#define AIMEE_WS_IO_HEADER_LEN      12u
#define AIMEE_WS_IO_RESP_HEADER_LEN 12u
#define AIMEE_WS_IO_PAYLOAD_MAX     (1u << 20)
#define AIMEE_WS_IO_MORE            1u /* another chunk of this result follows */

#define AIMEE_WS_IO_OP_SUBMIT          1u
#define AIMEE_WS_IO_OP_POLL            2u
#define AIMEE_WS_IO_OP_RESPOND         3u
#define AIMEE_WS_IO_OP_RESPOND_PARTIAL 4u
#define AIMEE_WS_IO_OP_DRAIN           5u

/* Encode a runner question about `value` (a tree to register/forget, or a path
 * to resolve). Returns 0, or -1 when the value cannot fit the frame. */
static inline int aimee_ws_runner_request_encode(unsigned op, const char *value, size_t value_len,
                                                 uint8_t *out, size_t cap)
{
   if (!out || cap < AIMEE_WS_RUNNER_REQUEST_LEN || !value || value_len == 0 ||
       value_len > AIMEE_WS_RUNNER_PATH_MAX)
      return -1;
   /* A registered id is the key the handoff is looked up by, so it must fit
    * that key; a path merely being asked about is only ever compared. */
   if ((op == AIMEE_WS_RUNNER_OP_REGISTER || op == AIMEE_WS_RUNNER_OP_UNREGISTER) &&
       value_len > AIMEE_WS_RUNNER_ID_MAX)
      return -1;
   memset(out, 0, AIMEE_WS_RUNNER_REQUEST_LEN);
   aimee_workspace_put_u32(out, AIMEE_WS_RUNNER_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_WORKSPACE_WIRE_VERSION;
   out[5] = (uint8_t)op;
   aimee_workspace_put_u16(out + 6, (uint16_t)value_len);
   memcpy(out + 8, value, value_len);
   return 0;
}

/* Decode the id of the client serving the tree that was asked about. An empty
 * id means nobody is serving it, which is an answer and not an error. */
static inline int aimee_ws_runner_response_decode(const uint8_t *in, size_t len, char *id,
                                                  size_t id_cap)
{
   if (!in || len != AIMEE_WS_RUNNER_RESPONSE_LEN || !id || id_cap == 0 ||
       aimee_workspace_get_u32(in) != AIMEE_WS_RUNNER_RESPONSE_MAGIC)
      return -1;
   uint32_t id_len = aimee_workspace_get_u32(in + 4);
   if (id_len > AIMEE_WS_RUNNER_ID_MAX || (size_t)id_len >= id_cap)
      return -1;
   memcpy(id, in + 8, id_len);
   id[id_len] = '\0';
   return 0;
}

/* Frame one handoff op for tree `id`, carrying `payload` bytes. Returns the
 * encoded length, or 0 when it does not fit. */
static inline size_t aimee_ws_io_request_encode(unsigned op, const char *id, const void *payload,
                                                size_t payload_len, uint8_t *out, size_t cap)
{
   size_t id_len = id ? strlen(id) : 0;
   if (!out || id_len == 0 || id_len > AIMEE_WS_RUNNER_ID_MAX ||
       payload_len > AIMEE_WS_IO_PAYLOAD_MAX || cap < AIMEE_WS_IO_HEADER_LEN + id_len + payload_len)
      return 0;
   aimee_workspace_put_u32(out, AIMEE_WS_IO_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_WORKSPACE_WIRE_VERSION;
   out[5] = (uint8_t)op;
   aimee_workspace_put_u16(out + 6, (uint16_t)id_len);
   aimee_workspace_put_u32(out + 8, (uint32_t)payload_len);
   memcpy(out + AIMEE_WS_IO_HEADER_LEN, id, id_len);
   if (payload_len)
      memcpy(out + AIMEE_WS_IO_HEADER_LEN + id_len, payload, payload_len);
   return AIMEE_WS_IO_HEADER_LEN + id_len + payload_len;
}

/* Point *payload at the returned chunk (borrowed from `in`) and report whether
 * another chunk follows. */
static inline int aimee_ws_io_response_decode(const uint8_t *in, size_t len,
                                              const uint8_t **payload, size_t *payload_len,
                                              int *more)
{
   if (!in || len < AIMEE_WS_IO_RESP_HEADER_LEN || !payload || !payload_len ||
       aimee_workspace_get_u32(in) != AIMEE_WS_IO_RESPONSE_MAGIC)
      return -1;
   uint32_t flags = aimee_workspace_get_u32(in + 4);
   uint32_t body = aimee_workspace_get_u32(in + 8);
   if (len != AIMEE_WS_IO_RESP_HEADER_LEN + (size_t)body)
      return -1;
   *payload = in + AIMEE_WS_IO_RESP_HEADER_LEN;
   *payload_len = body;
   if (more)
      *more = (flags & AIMEE_WS_IO_MORE) != 0;
   return 0;
}

static inline int aimee_workspace_response_decode(const uint8_t *in, size_t len, int *allowed)
{
   if (!in || len != AIMEE_WORKSPACE_RESPONSE_LEN || !allowed ||
       aimee_workspace_get_u32(in) != AIMEE_WORKSPACE_RESPONSE_MAGIC ||
       aimee_workspace_get_u32(in + 4) > 1u)
      return -1;
   *allowed = (int)aimee_workspace_get_u32(in + 4);
   return 0;
}

#endif
