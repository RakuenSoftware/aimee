/* jti_replay_consume.c: the caller's side of the two replay checks.
 *
 * The served operation takes the token and the instant as one row; these are
 * the two-argument forms the callers already use.
 *
 * A call that does not complete is reported as STORAGE, not as some value
 * outside the enum. STORAGE is the domain's own word for "the store did not
 * record this jti", which is exactly true when the module could not be reached,
 * and every caller already denies on it. Handing back -1 would rely on a switch
 * with no default falling through to a deny -- which is what happens today, and
 * is one added `default:` away from admitting a token whose replay check never
 * ran.
 */
#include <string.h>

#include "server_identity_jti.h"
#include "server_management_jti.h"

server_identity_jti_result_t db1_identity_jti_consume(const server_identity_jti_t *token,
                                                      int64_t consumed_at)
{
   if (!token)
      return SERVER_IDENTITY_JTI_INVALID;
   db1_identity_jti_consume_t in;
   memset(&in, 0, sizeof in);
   in.token = *token;
   in.consumed_at = consumed_at;
   server_identity_jti_result_t rc = db1_identity_jti_consume_row(&in);
   return (rc < SERVER_IDENTITY_JTI_OK || rc > SERVER_IDENTITY_JTI_INVALID)
              ? SERVER_IDENTITY_JTI_STORAGE
              : rc;
}

server_management_jti_result_t db1_management_jti_consume(const server_management_jti_t *token,
                                                          int64_t consumed_at)
{
   if (!token)
      return SERVER_MANAGEMENT_JTI_INVALID;
   db1_management_jti_consume_t in;
   memset(&in, 0, sizeof in);
   in.token = *token;
   in.consumed_at = consumed_at;
   server_management_jti_result_t rc = db1_management_jti_consume_row(&in);
   return (rc < SERVER_MANAGEMENT_JTI_OK || rc > SERVER_MANAGEMENT_JTI_INVALID)
              ? SERVER_MANAGEMENT_JTI_STORAGE
              : rc;
}
