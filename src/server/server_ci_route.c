/* server_ci_route.c: the PC2 CI-event webhook handler (POST /v1/dev/ci-event),
 * extracted from server_http_routes.c to keep that file under the line-count limit.
 * See server_http_internal.h for route_req_t / rh_dev_ci_event. */
#include "server_http_internal.h"
#include "server.h"        /* server_ct_equal */
#include "wfe_scheduler.h" /* wfe_scheduler_notify */
#include "wfe_store.h"     /* db1_work_item_* / db1_lifecycle_event_* / by_pr_ref */
#include "json_fluent.h"   /* jo_cstr */
#include "cJSON.h"
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HMAC-SHA256(secret, msg) -> lowercase hex (65 bytes) via libcrypto (the server
 * already links OpenSSL for TLS; mirrors platform_webhook.c). Empty on failure. */
static void ci_hmac_sha256_hex(const char *secret, const char *msg, char out_hex[65])
{
   out_hex[0] = '\0';
   unsigned char mac[32];
   unsigned int maclen = 0;
   if (!HMAC(EVP_sha256(), secret, (int)(secret ? strlen(secret) : 0),
             (const unsigned char *)(msg ? msg : ""), msg ? strlen(msg) : 0, mac, &maclen) ||
       maclen != 32)
      return;
   static const char hx[] = "0123456789abcdef";
   for (int i = 0; i < 32; i++)
   {
      out_hex[i * 2] = hx[mac[i] >> 4];
      out_hex[i * 2 + 1] = hx[mac[i] & 0xf];
   }
   out_hex[64] = '\0';
}

/* POST /v1/dev/ci-event {pr_ref, head_sha, status, log_url?, signature} — a
 * system-to-system CI webhook (PC2/Q3). It records the CI outcome for the work item
 * that owns `pr_ref` and immediately resumes the autonomy scheduler (vs waiting for
 * the 30s backstop sweep). Integrity: HMAC-SHA256 over "pr_ref|head_sha|status" keyed
 * by AIMEE_CI_WEBHOOK_SECRET (fail-CLOSED 503 if the secret is unset — a shared dev
 * bearer is the wrong primitive for a machine webhook). Idempotent: a replayed
 * (pr_ref, head_sha, status) is recorded once. status ∈ passed|failed|error|pending. */
int rh_dev_ci_event(const route_req_t *rq, char *resp, int cap)
{
   const char *secret = getenv("AIMEE_CI_WEBHOOK_SECRET");
   if (!secret || !secret[0])
   {
      snprintf(resp, cap,
               "{\"error\":\"ci-event webhook disabled: AIMEE_CI_WEBHOOK_SECRET unset\"}");
      return 503; /* fail-closed */
   }
   /* This route has no capability gate (a machine caller has no attested principal;
    * the HMAC is its auth). Bound the body BEFORE parsing so an unauthenticated caller
    * cannot drive unbounded JSON parsing — a CI event is tiny. */
   if (rq->body_len > 8192)
   {
      snprintf(resp, cap, "{\"error\":\"ci-event body too large\"}");
      return 413;
   }
   cJSON *body = rq->body ? cJSON_Parse(rq->body) : NULL;
   const char *pr_ref = body ? jo_cstr(body, "pr_ref") : NULL;
   const char *head_sha = body ? jo_cstr(body, "head_sha") : NULL;
   const char *status = body ? jo_cstr(body, "status") : NULL;
   const char *sig = body ? jo_cstr(body, "signature") : NULL;
   const char *log_url = body ? jo_cstr(body, "log_url") : NULL;
   if (!pr_ref || !pr_ref[0] || !head_sha || !head_sha[0] || !status || !status[0] || !sig ||
       !sig[0])
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"pr_ref, head_sha, status, signature required\"}");
      return 400;
   }
   /* Bound the fields so the canonical HMAC message never SILENTLY TRUNCATES (a
    * truncated HMAC input diverges from a client that signed the full fields ->
    * confusing auth failures) and the dedup key stays well-formed. */
   if (strlen(pr_ref) >= 256 || strlen(head_sha) >= 128 || strlen(status) >= 16)
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"pr_ref/head_sha/status too long\"}");
      return 400;
   }
   /* status enum (unknown -> 400) */
   if (strcmp(status, "passed") != 0 && strcmp(status, "failed") != 0 &&
       strcmp(status, "error") != 0 && strcmp(status, "pending") != 0)
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"status must be passed|failed|error|pending\"}");
      return 400;
   }
   /* integrity: HMAC over the canonical "pr_ref|head_sha|status" (fields length-
    * bounded above, so this cannot truncate). */
   char canon[512];
   snprintf(canon, sizeof canon, "%s|%s|%s", pr_ref, head_sha, status);
   char expect[65];
   ci_hmac_sha256_hex(secret, canon, expect);
   if (!expect[0] || !server_ct_equal(sig, expect))
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"invalid signature\"}");
      return 401;
   }
   /* route to the work item owning this PR ref */
   char wid[80] = "";
   int r = db1_work_item_id_by_pr_ref(pr_ref, wid, sizeof wid);
   if (r != 1 || !wid[0])
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"no work item for pr_ref\"}");
      return 404;
   }
   db1_work_item_t wi;
   if (db1_work_item_get(wid, &wi) != 1)
   {
      cJSON_Delete(body);
      snprintf(resp, cap, "{\"error\":\"work item unreadable\"}");
      return 404;
   }
   /* dedup on (status, head_sha): a replayed event is recorded once (idempotent). The
    * dedup prefix includes the TRAILING '|' delimiter so a head_sha that is a string
    * prefix of another cannot false-positive as a duplicate. (A later event with a
    * DIFFERENT status — e.g. a delayed "passed" after "failed" — is a distinct key, so
    * it is recorded and wfe_last_ci_outcome's latest-wins read reflects it.) */
   char detail[600];
   snprintf(detail, sizeof detail, "%s|%s|%s", status, head_sha, log_url ? log_url : "");
   char dup_prefix[300];
   snprintf(dup_prefix, sizeof dup_prefix, "%s|%s|", status, head_sha);
   int duplicate = 0;
   db1_lifecycle_event_t *evs = NULL;
   int nev = db1_lifecycle_event_list(wid, &evs);
   for (int i = 0; i < nev; i++)
      if (strcmp(evs[i].kind, "ci_event") == 0 &&
          strncmp(evs[i].detail, dup_prefix, strlen(dup_prefix)) == 0)
      {
         duplicate = 1;
         break;
      }
   free(evs);
   if (!duplicate)
   {
      db1_lifecycle_event_add(wid, wi.current_stage, "ci_event", "ci", detail, "", 0);
      wfe_scheduler_notify(); /* resume the run immediately (vs the 30s sweep) */
   }
   cJSON_Delete(body);
   snprintf(resp, cap, "{\"ok\":true,\"work_item\":\"%s\",\"duplicate\":%s}", wid,
            duplicate ? "true" : "false");
   return 200;
}
