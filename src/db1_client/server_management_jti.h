#ifndef DEC_SERVER_MANAGEMENT_JTI_H
#define DEC_SERVER_MANAGEMENT_JTI_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SERVER_MANAGEMENT_JTI_LIVE_LIMIT 4096

   typedef struct
   {
      const char *jti;
      const char *issuer;
      const char *kid;
      const char *audience;
      const char *subject;
      int64_t team_id;
      const char *capability;
      const char *peer_issuer;
      const char *peer_serial;
      const char *peer_fingerprint;
      const char *request_sha256;
      const char *correlation_id;
      int64_t issued_at;
      int64_t expires_at;
   } server_management_jti_t;

   typedef enum
   {
      SERVER_MANAGEMENT_JTI_OK = 0,
      SERVER_MANAGEMENT_JTI_REPLAY = 1,
      SERVER_MANAGEMENT_JTI_SATURATED = 2,
      SERVER_MANAGEMENT_JTI_STORAGE = 3,
      SERVER_MANAGEMENT_JTI_INVALID = 4
   } server_management_jti_result_t;

   /* Atomically consume a fully verified management token. The row is durable before OK is
    * returned and is never removed on a downstream failure. `consumed_at` is supplied by the
    * caller so authorization and replay tests use the same clock instant. */
   /* The token and the instant it was consumed, as one input.
    *
    * consume() takes them as two arguments, and a request that is a struct is
    * the whole request -- so the pair travels as one row and the two-argument
    * form below unpacks it. The clock stays the caller's either way, which is
    * the point of passing it in: authorization and the replay check must agree
    * on the instant. */
   typedef struct
   {
      server_management_jti_t token;
      int64_t consumed_at;
   } db1_management_jti_consume_t;

   server_management_jti_result_t
   db1_management_jti_consume_row(const db1_management_jti_consume_t *in);

   server_management_jti_result_t db1_management_jti_consume(const server_management_jti_t *token,
                                                             int64_t consumed_at);

#ifdef SERVER_MANAGEMENT_JTI_TEST_API
   /* Focused-test entry point. It is absent from the production header surface. */
   server_management_jti_result_t
   server_management_jti_consume_for_test(const server_management_jti_t *token, int64_t consumed_at,
                                          size_t live_limit);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_MANAGEMENT_JTI_H */
