#ifndef AIMEE_CORE_CONNECTION_AUTH_H
#define AIMEE_CORE_CONNECTION_AUTH_H 1

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

   /* Constant-time equality for NUL-terminated credentials. Both strings are
    * scanned to the longer length; NULL is treated as an empty string. */
   int aimee_core_credential_equal(const char *left, const char *right);

   /* Return the token in an RFC 7235 Bearer value, or NULL when the value is
    * absent, malformed, or contains whitespace inside the credential. The
    * scheme match is ASCII case-insensitive. The returned pointer aliases
    * authorization_value. */
   const char *aimee_core_bearer_token(const char *authorization_value);

   /* Length-bounded form for HTTP parsers that have not copied the field value
    * into a NUL-terminated buffer. On success, token_out aliases value. */
   int aimee_core_bearer_token_span(const char *value, size_t value_len, const char **token_out,
                                    size_t *token_len_out);

   /* Format one complete Authorization header value (without the field name).
    * Returns 0 on success and -1 for an empty token or insufficient output. */
   int aimee_core_bearer_value(char *out, size_t out_size, const char *token);

   /* The one cleartext credential rule used by every Aimee network client. */
   int aimee_core_host_is_loopback(const char *host);
   int aimee_core_would_leak_credential(int transport_is_secure, const char *host,
                                        const char *credential);

#ifdef __cplusplus
}
#endif

#endif
