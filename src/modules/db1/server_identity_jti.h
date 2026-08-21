#ifndef DEC_SERVER_IDENTITY_JTI_H
#define DEC_SERVER_IDENTITY_JTI_H 1

/* Replay store for the data-plane identity token (proposal §4/§9).
 *
 * A sibling of server_management_jti, not a reuse of it: that record requires a
 * peer certificate and a request digest, which an identity token does not have.
 * See the table comment in db1/schema.sql for why sharing would be worse than
 * duplicating. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SERVER_IDENTITY_JTI_LIVE_LIMIT 4096

   typedef struct
   {
      const char *jti;
      const char *issuer;
      const char *kid;
      const char *audience;
      const char *subject;
      int64_t team_id;
      const char *tier; /* "off" | "data" | "full" */
      int64_t issued_at;
      int64_t expires_at;
   } server_identity_jti_t;

   typedef enum
   {
      SERVER_IDENTITY_JTI_OK = 0,
      SERVER_IDENTITY_JTI_REPLAY = 1,
      SERVER_IDENTITY_JTI_SATURATED = 2,
      SERVER_IDENTITY_JTI_STORAGE = 3,
      SERVER_IDENTITY_JTI_INVALID = 4
   } server_identity_jti_result_t;

   /* Atomically consume a fully verified identity token. The row is durable
    * before OK is returned and is never removed on a downstream failure, so a
    * token cannot be replayed by making a later stage fail. */
   /* The token and the instant it was consumed, as one input.
    *
    * consume() takes them as two arguments, and a request that is a struct is
    * the whole request -- so the pair travels as one row and the two-argument
    * form below unpacks it. The clock stays the caller's either way, which is
    * the point of passing it in: authorization and the replay check must agree
    * on the instant. */
   typedef struct
   {
      server_identity_jti_t token;
      int64_t consumed_at;
   } db1_identity_jti_consume_t;

   server_identity_jti_result_t db1_identity_jti_consume_row(const db1_identity_jti_consume_t *in);

   server_identity_jti_result_t db1_identity_jti_consume(const server_identity_jti_t *token,
                                                         int64_t consumed_at);

#ifdef SERVER_IDENTITY_JTI_TEST_API
   server_identity_jti_result_t
   server_identity_jti_consume_for_test(const server_identity_jti_t *token, int64_t consumed_at,
                                        size_t live_limit);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DEC_SERVER_IDENTITY_JTI_H */
