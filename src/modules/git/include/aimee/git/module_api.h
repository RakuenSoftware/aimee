/* Wire contract for webchat git-operation classification. */
#ifndef AIMEE_GIT_MODULE_API_H
#define AIMEE_GIT_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_GIT_EVENT_OPERATION    7425u
#define AIMEE_GIT_EVENT_REF_VALIDATE 7426u
#define AIMEE_GIT_EVENT_CI_GRADE     7427u
#define AIMEE_GIT_STAGE_OPERATION    1u
#define AIMEE_GIT_STAGE_REF_VALIDATE 2u
/* CI grading carries JSON rather than the fixed binary framing the other two
 * stages use: a forge's check-runs payload is arbitrarily large and its shape is
 * the forge's, not ours. */
#define AIMEE_GIT_STAGE_CI_GRADE     3u
/* Stages 4-6 are the destination for the I/O that still lives in C: the forge
 * HTTP client, credential resolution, and the verify pipeline. They are declared
 * with the rest of the wire contract so the port lands one caller at a time
 * against a fixed stage table rather than renumbering as it goes. Each carries
 * JSON, for the same reason CI grading does: the payload shape is the forge's. */
#define AIMEE_GIT_EVENT_FORGE_REQUEST 7428u
#define AIMEE_GIT_EVENT_CRED_RESOLVE  7429u
#define AIMEE_GIT_EVENT_VERIFY_RUN    7430u
#define AIMEE_GIT_STAGE_FORGE_REQUEST 4u
#define AIMEE_GIT_STAGE_CRED_RESOLVE  5u
#define AIMEE_GIT_STAGE_VERIFY_RUN    6u
#define AIMEE_GIT_REQUEST_MAGIC      0x53504f47u /* "GOPS" */
#define AIMEE_GIT_RESPONSE_MAGIC     0x534c4347u /* "GCLS" */
#define AIMEE_GIT_REF_REQUEST_MAGIC  0x46455247u /* "GREF" */
#define AIMEE_GIT_REF_RESPONSE_MAGIC 0x4c415647u /* "GVAL" */
#define AIMEE_GIT_WIRE_VERSION       1u
#define AIMEE_GIT_OP_MAX             15u
#define AIMEE_GIT_REQUEST_LEN        24u
#define AIMEE_GIT_RESPONSE_LEN       12u
#define AIMEE_GIT_REF_MAX            200u
#define AIMEE_GIT_REF_REQUEST_LEN    208u
#define AIMEE_GIT_REF_RESPONSE_LEN   8u

typedef enum
{
   AIMEE_GIT_OP_UNSUPPORTED = 0,
   AIMEE_GIT_OP_STATUS,
   AIMEE_GIT_OP_LOG,
   AIMEE_GIT_OP_DIFF,
   AIMEE_GIT_OP_BRANCH,
   AIMEE_GIT_OP_FETCH,
   AIMEE_GIT_OP_PULL,
   AIMEE_GIT_OP_PUSH,
   AIMEE_GIT_OP_CHECKOUT,
   AIMEE_GIT_OP_COMMIT,
   AIMEE_GIT_OP_PR
} aimee_git_operation_t;

typedef struct
{
   aimee_git_operation_t operation;
   int needs_credentials;
} aimee_git_classification_t;

static inline void aimee_git_put_u32(uint8_t *p, uint32_t v)
{
   for (unsigned i = 0; i < 4; ++i)
      p[i] = (uint8_t)(v >> (8u * i));
}

static inline uint32_t aimee_git_get_u32(const uint8_t *p)
{
   uint32_t v = 0;
   for (unsigned i = 0; i < 4; ++i)
      v |= (uint32_t)p[i] << (8u * i);
   return v;
}

static inline void aimee_git_put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)v;
   p[1] = (uint8_t)(v >> 8u);
}

static inline uint16_t aimee_git_get_u16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8u));
}

static inline int aimee_git_request_encode(const char *op, uint8_t *out, size_t cap)
{
   size_t len = op ? strlen(op) : 0;
   if (!out || cap < AIMEE_GIT_REQUEST_LEN || len == 0 || len > AIMEE_GIT_OP_MAX)
      return -1;
   memset(out, 0, AIMEE_GIT_REQUEST_LEN);
   aimee_git_put_u32(out, AIMEE_GIT_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_GIT_WIRE_VERSION;
   out[6] = (uint8_t)len;
   memcpy(out + 8, op, len);
   return 0;
}

static inline int aimee_git_request_decode(const uint8_t *in, size_t len, char *op, size_t cap)
{
   if (!in || len != AIMEE_GIT_REQUEST_LEN || !op || cap == 0 ||
       aimee_git_get_u32(in) != AIMEE_GIT_REQUEST_MAGIC || in[4] != AIMEE_GIT_WIRE_VERSION ||
       in[5] != 0 || in[7] != 0 || in[6] == 0 || in[6] > AIMEE_GIT_OP_MAX || (size_t)in[6] >= cap)
      return -1;
   memcpy(op, in + 8, in[6]);
   op[in[6]] = '\0';
   return 0;
}

static inline int aimee_git_response_decode(const uint8_t *in, size_t len,
                                            aimee_git_classification_t *out)
{
   if (!in || len != AIMEE_GIT_RESPONSE_LEN || !out ||
       aimee_git_get_u32(in) != AIMEE_GIT_RESPONSE_MAGIC ||
       aimee_git_get_u32(in + 4) > AIMEE_GIT_OP_PR || aimee_git_get_u32(in + 8) > 1u)
      return -1;
   out->operation = (aimee_git_operation_t)aimee_git_get_u32(in + 4);
   out->needs_credentials = (int)aimee_git_get_u32(in + 8);
   return 0;
}

static inline int aimee_git_ref_request_encode(const char *ref, uint8_t *out, size_t cap)
{
   size_t len = ref ? strlen(ref) : 0;
   if (!out || cap < AIMEE_GIT_REF_REQUEST_LEN || len == 0 || len > AIMEE_GIT_REF_MAX)
      return -1;
   memset(out, 0, AIMEE_GIT_REF_REQUEST_LEN);
   aimee_git_put_u32(out, AIMEE_GIT_REF_REQUEST_MAGIC);
   out[4] = (uint8_t)AIMEE_GIT_WIRE_VERSION;
   aimee_git_put_u16(out + 6, (uint16_t)len);
   memcpy(out + 8, ref, len);
   return 0;
}

static inline int aimee_git_ref_request_decode(const uint8_t *in, size_t len, char *ref, size_t cap)
{
   uint16_t ref_len;
   if (!in || len != AIMEE_GIT_REF_REQUEST_LEN || !ref || cap == 0 ||
       aimee_git_get_u32(in) != AIMEE_GIT_REF_REQUEST_MAGIC || in[4] != AIMEE_GIT_WIRE_VERSION ||
       in[5] != 0)
      return -1;
   ref_len = aimee_git_get_u16(in + 6);
   if (ref_len == 0 || ref_len > AIMEE_GIT_REF_MAX || (size_t)ref_len >= cap ||
       memchr(in + 8, '\0', ref_len) != NULL)
      return -1;
   memcpy(ref, in + 8, ref_len);
   ref[ref_len] = '\0';
   return 0;
}

static inline int aimee_git_ref_response_decode(const uint8_t *in, size_t len, int *allowed)
{
   if (!in || len != AIMEE_GIT_REF_RESPONSE_LEN || !allowed ||
       aimee_git_get_u32(in) != AIMEE_GIT_REF_RESPONSE_MAGIC || aimee_git_get_u32(in + 4) > 1u)
      return -1;
   *allowed = (int)aimee_git_get_u32(in + 4);
   return 0;
}

#endif
