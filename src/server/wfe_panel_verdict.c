/* wfe_panel_verdict.c -- map a panel reviewer's reply to a roundtable verdict. */
#include "wfe_panel_verdict.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

void wfe_panel_verdict_from_review(const char *persona, const char *artifact_hash,
                                   const char *review_text, wfe_verdict_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   snprintf(out->persona, sizeof out->persona, "%s", persona ? persona : "");
   out->schema_version = WFE_VERDICT_SCHEMA;
   snprintf(out->reviewed_content_hash, sizeof out->reviewed_content_hash, "%s",
            artifact_hash ? artifact_hash : "");
   out->kind = WFE_V_MALFORMED; /* fail-closed default: never approve on bad input */
   out->high_sev_blockers = 0;

   if (!review_text || !review_text[0])
      return;

   /* Extract ONLY the last non-empty line (the delegate emits one JSON line at the
    * end). A whole-response scan could false-approve on quoted JSON in the reasoning.
    * Providers routinely wrap that final JSON in a markdown code fence, leaving
    * "```" as the literal last line — skip trailing fence-only lines (``` or
    * ```json) so a fenced verdict isn't misread as MALFORMED, but never skip past
    * anything else. */
   const char *r = review_text;
   size_t len = strlen(r);
   size_t start;
   for (;;)
   {
      while (len > 0 &&
             (r[len - 1] == '\n' || r[len - 1] == '\r' || r[len - 1] == ' ' || r[len - 1] == '\t'))
         len--;
      start = len;
      while (start > 0 && r[start - 1] != '\n')
         start--;
      size_t l = len - start;
      if (l >= 3 && l <= 16 && strncmp(r + start, "```", 3) == 0)
      {
         len = start; /* fence-only line: consider the line above instead */
         continue;
      }
      break;
   }

   /* Capture the reviewer's critique — everything before the final JSON verdict
    * line — so a request_changes can be threaded back to the re-authoring
    * delegate. Keep the TAIL when it overflows the buffer: reviewers tend to
    * conclude with their concrete blockers just above the verdict line. Done for
    * every kind (incl. MALFORMED, which the gate coerces to request_changes). */
   size_t body = start;
   while (body > 0 &&
          (r[body - 1] == '\n' || r[body - 1] == '\r' || r[body - 1] == ' ' || r[body - 1] == '\t'))
      body--;
   if (body > 0)
   {
      size_t keep = body, off = 0;
      if (keep >= sizeof out->feedback)
      {
         keep = sizeof out->feedback - 1;
         off = body - keep;
      }
      memcpy(out->feedback, r + off, keep);
      out->feedback[keep] = '\0';
   }

   size_t llen = len - start;
   char line[1024];
   if (llen == 0 || llen >= sizeof line)
      return; /* no parseable last line -> MALFORMED */
   memcpy(line, r + start, llen);
   line[llen] = '\0';

   cJSON *doc = cJSON_Parse(line);
   if (!doc)
      return; /* unparseable -> MALFORMED */
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(doc, "verdict");
   if (v && cJSON_IsString(v) && v->valuestring)
   {
      if (strcmp(v->valuestring, "approve") == 0)
         out->kind = WFE_V_APPROVE;
      else if (strcmp(v->valuestring, "request_changes") == 0)
         out->kind = WFE_V_REQUEST_CHANGES;
      else if (strcmp(v->valuestring, "comment") == 0)
         out->kind = WFE_V_COMMENT;
      /* any other string stays MALFORMED (fail closed) */
   }
   const cJSON *hs = cJSON_GetObjectItemCaseSensitive(doc, "high_sev_blockers");
   if (hs && cJSON_IsNumber(hs) && hs->valueint > 0)
      out->high_sev_blockers = hs->valueint;
   cJSON_Delete(doc);
}
