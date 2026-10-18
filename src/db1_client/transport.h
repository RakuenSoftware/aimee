#ifndef AIMEE_DB1_CLIENT_TRANSPORT_H
#define AIMEE_DB1_CLIENT_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

/* A descriptor owns only transport addressing and its log throttle. */
typedef struct
{
   const char *tag;
   const char *name;
   uint32_t event_kind;
   uint32_t stage_id;
   uint64_t timeout_ms;
   atomic_int warned;
} db1_family_t;

/* Writes return the module status. Fixed replies require exactly slots values;
 * lists pass filled_out for a variable count. Output buffers are caller-owned. */
int db1_call(db1_family_t *family, uint32_t op, const char *const *fields, uint32_t count,
             char *const *values, const size_t *caps, uint32_t slots, uint32_t *filled_out);
#endif
