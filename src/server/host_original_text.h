#ifndef HOST_ORIGINAL_TEXT_H
#define HOST_ORIGINAL_TEXT_H
#include <stddef.h>
#include <stdint.h>
#define HOST_TEXT_MAX_INPUT    262144u
#define HOST_TEXT_MAX_OUTPUT   262144u
#define HOST_TEXT_MAX_SIDECARS 262144u
typedef enum
{
   HOST_TEXT_PLAIN_PROMPT = 1,
   HOST_TEXT_EXTERNAL_TRANSCRIPT = 2
} host_text_kind;
typedef enum
{
   HOST_TEXT_OK = 0,
   HOST_TEXT_PASSTHROUGH = 1,
   /* Existing input receipt only; never a new source or completed model turn. */
   HOST_TEXT_ALREADY_DELIVERED = 2,
   HOST_TEXT_UNAVAILABLE = -1,
   HOST_TEXT_UNSUPPORTED = -2,
   HOST_TEXT_REFUSED = -3,
   HOST_TEXT_UNKNOWN = -4,
   HOST_TEXT_INVALID = -5
} host_text_status;
typedef struct
{
   uint32_t schema_version;
   host_text_kind kind;
   char operation_uuid[37], principal[128], session_id[129], transport_identity[129];
} host_text_origin;
/* Trusted startup resolver, not a role label or JSON identity. Nonzero refuses. */
typedef host_text_status (*host_text_resolve_fn)(host_text_kind, host_text_origin *, void *);
/* No raw retention/async use. Fixed host-owned output buffers; all-or-nothing. */
typedef host_text_status (*host_text_transform_fn)(const host_text_origin *, const char *, size_t,
                                                   char *, size_t, size_t *, char *, size_t,
                                                   size_t *, void *);
typedef struct
{
   char *text, *sidecars_json;
   size_t text_len, sidecars_len;
   int transformed;
} host_text_owned;
int host_text_install(int required, host_text_resolve_fn, host_text_transform_fn, void *);
int host_text_required(void);
host_text_status host_text_apply(host_text_kind, const char *, size_t, host_text_owned *);
void host_text_destroy(host_text_owned *);
#endif
