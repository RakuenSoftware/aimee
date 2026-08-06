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

/* The role alias -> canonical role map: THE single source of truth.
 *
 * Two callers need it and must never disagree: the module handler that answers
 * AIMEE_DELEGATES_STAGE_INVOKE over the event bus, and delegate_role.c's local
 * path for binaries that host no bus (the thin client) and so have no module to
 * call. Both previously carried their own byte-identical copy of this table with
 * nothing keeping them in sync — a silent drift would have made the same role
 * canonicalize differently depending on which binary ran it. Adding an alias
 * here now changes both by construction.
 *
 * Returns `role` itself when it is already canonical or unknown. */
static inline const char *aimee_delegates_role_canonical(const char *role)
{
   static const struct
   {
      const char *alias;
      const char *canonical;
   } aliases[] = {
       {"implement", "code"},        {"build", "code"},
       {"reviewer", "review"},       {"verifier", "validate"},
       {"test", "validate"},         {"check", "validate"},
       {"evaluate", "validate"},     {"evaluate-optimize", "validate"},
       {"inspect", "diagnose"},      {"research", "execute"},
       {"enforce", "execute"},       {"recall", "search"},
       {"synthesize", "summarize"},  {"rank-fuse", "reason"},
       {"classify-score", "reason"}, {"planner", "plan"},
       {"planning", "plan"},         {NULL, NULL},
   };
   if (!role)
      return role;
   for (size_t i = 0; aliases[i].alias; ++i)
      if (strcmp(role, aliases[i].alias) == 0)
         return aliases[i].canonical;
   return role;
}

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

#endif
