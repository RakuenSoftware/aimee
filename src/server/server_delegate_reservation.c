/* server/server_delegate_reservation.c: release a delegate replay reservation.
 *
 * The reservation itself is taken on the launch path (handle_delegate), where
 * the job id is created. Releasing it is a separate route because the decision
 * is the caller's, not this server's: whether a partial result is replayable
 * depends on what the calling block required of it, and that policy is not
 * visible here. C owns the storage; the caller owns the judgement.
 *
 * job_id is optional and makes the release a compare-delete. A caller that
 * cancelled a specific job must pass it, so a retry that has already reserved a
 * newer job under the same key is not erased by the older job's cleanup. */

#include "server.h"

#include "cJSON.h"
#include "db1/delegate_reservation.h"
#include "json_fluent.h" /* jo_ok */

int handle_delegate_reservation_forget(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jkey = cJSON_GetObjectItemCaseSensitive(req, "execution_key");
   if (!cJSON_IsString(jkey) || !jkey->valuestring[0])
      return server_send_error(conn, "execution_key is required", NULL);

   cJSON *jjob = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   int released;
   if (cJSON_IsNumber(jjob) && jjob->valueint > 0)
      released = db1_delegate_reservation_forget_if_matches(jkey->valuestring, jjob->valueint);
   else
      released = db1_delegate_reservation_forget(jkey->valuestring) == 0 ? 1 : -1;

   if (released < 0)
      return server_send_error(conn, "failed to release the delegate reservation", NULL);

   cJSON *resp = jo_ok();
   /* False means the reservation was already gone or now names a different job.
    * Both are terminal for this caller, so neither is an error -- but the caller
    * must be able to tell "I released it" from "someone else owns it now". */
   cJSON_AddBoolToObject(resp, "released", released == 1);
   return server_send_ok(conn, resp);
}
