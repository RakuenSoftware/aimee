/* remote_client_claim.c: the caller's side of a first-user claim.
 *
 * db1_remote_client_claim_row answers the grant and the verdict together,
 * because they are one decision: whether this principal created the slot,
 * re-entered its own, or was refused because somebody else owns it. This
 * unpacks that into the enum the callers already switch on.
 *
 * A store that cannot answer is reported as STORAGE, which is what the domain
 * returns when its own query fails -- the caller refuses the request either
 * way, and inventing a different value here would make a broken database look
 * like a contested claim.
 */
#include <string.h>

#include "remote_client_grant.h"

db1_remote_client_claim_result_t db1_remote_client_claim(const char *principal,
                                                         const char *new_bearer_sha256, int64_t now,
                                                         db1_remote_client_grant_t *out)
{
   db1_remote_client_claim_row_t row;
   memset(&row, 0, sizeof row);
   if (db1_remote_client_claim_row(principal, new_bearer_sha256, now, &row) != 0)
      return DB1_REMOTE_CLIENT_CLAIM_STORAGE;
   if (out)
      *out = row.grant;
   return (db1_remote_client_claim_result_t)row.result;
}
