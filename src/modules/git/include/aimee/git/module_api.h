/* Wire contract for webchat git-operation classification. */
#ifndef AIMEE_GIT_MODULE_API_H
#define AIMEE_GIT_MODULE_API_H 1

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AIMEE_GIT_EVENT_OPERATION   7425u
#define AIMEE_GIT_STAGE_OPERATION   1u
#define AIMEE_GIT_REQUEST_MAGIC     0x53504f47u /* "GOPS" */
#define AIMEE_GIT_RESPONSE_MAGIC    0x534c4347u /* "GCLS" */
#define AIMEE_GIT_WIRE_VERSION      1u
#define AIMEE_GIT_OP_MAX            15u
#define AIMEE_GIT_REQUEST_LEN       24u
#define AIMEE_GIT_RESPONSE_LEN      12u

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
       in[5] != 0 || in[7] != 0 || in[6] == 0 || in[6] > AIMEE_GIT_OP_MAX ||
       (size_t)in[6] >= cap)
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

#endif
