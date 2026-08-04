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
#include "db1/agent_jobs.h"
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

/* Cancel a delegate job that was never assigned to a worker.
 *
 * agent_jobs is this server's table, so the transition belongs here rather than
 * in a caller reaching across into it. The caller supplies the age it considers
 * expired, because how long an unassigned job may wait is its policy -- but
 * whether the row is still unassigned is decided here, atomically, so a worker
 * picking it up in the same instant wins instead of being cancelled underneath.
 */
int handle_delegate_cancel_unassigned(server_ctx_t *ctx, server_conn_t *conn, cJSON *req)
{
   (void)ctx;
   cJSON *jjob = cJSON_GetObjectItemCaseSensitive(req, "job_id");
   cJSON *jage = cJSON_GetObjectItemCaseSensitive(req, "min_unassigned_secs");
   cJSON *jreason = cJSON_GetObjectItemCaseSensitive(req, "reason");
   if (!cJSON_IsNumber(jjob) || jjob->valueint <= 0)
      return server_send_error(conn, "job_id is required", NULL);
   if (!cJSON_IsNumber(jage) || jage->valueint <= 0)
      return server_send_error(conn, "min_unassigned_secs is required", NULL);

   int cancelled = db1_agent_job_cancel_unassigned(
       jjob->valueint, cJSON_IsString(jreason) ? jreason->valuestring : NULL, jage->valueint);
   if (cancelled < 0)
      return server_send_error(conn, "failed to cancel the unassigned delegate job", NULL);

   cJSON *resp = jo_ok();
   /* False means the job was assigned, already terminal, or not yet old enough.
    * None of those are errors, but the caller must not read them as a
    * cancellation it can now rely on. */
   cJSON_AddBoolToObject(resp, "cancelled", cancelled == 1);
   return server_send_ok(conn, resp);
}
