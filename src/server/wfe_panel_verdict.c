/* wfe_panel_verdict.c -- map a panel reviewer's reply to a roundtable verdict,
 * and replay-verify a request_changes verdict's blocker citations against the
 * worktree under review. */
#include "wfe_panel_verdict.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

/* Copy the blockers array (if any) off the verdict JSON line. Entries without a
 * non-empty file and a positive line are interpretive (nothing to replay) and
 * are not stored; at most WFE_VERDICT_MAX_BLOCKERS are kept. */
static void parse_blockers(const cJSON *doc, wfe_verdict_t *out)
{
   const cJSON *bl = cJSON_GetObjectItemCaseSensitive(doc, "blockers");
   if (!bl || !cJSON_IsArray(bl))
      return;
   const cJSON *b = NULL;
   cJSON_ArrayForEach(b, bl)
   {
      if (out->blocker_count >= WFE_VERDICT_MAX_BLOCKERS)
         break;
      if (!cJSON_IsObject(b))
         continue;
      const cJSON *f = cJSON_GetObjectItemCaseSensitive(b, "file");
      const cJSON *ln = cJSON_GetObjectItemCaseSensitive(b, "line");
      if (!f || !cJSON_IsString(f) || !f->valuestring || !f->valuestring[0])
         continue;
      if (!ln || !cJSON_IsNumber(ln) || ln->valueint < 1)
         continue;
      wfe_blocker_t *o = &out->blockers[out->blocker_count];
      snprintf(o->file, sizeof o->file, "%s", f->valuestring);
      o->line = ln->valueint;
      const cJSON *q = cJSON_GetObjectItemCaseSensitive(b, "quote");
      if (q && cJSON_IsString(q) && q->valuestring)
         snprintf(o->quote, sizeof o->quote, "%s", q->valuestring);
      const cJSON *s = cJSON_GetObjectItemCaseSensitive(b, "summary");
      if (s && cJSON_IsString(s) && s->valuestring)
         snprintf(o->summary, sizeof o->summary, "%s", s->valuestring);
      out->blocker_count++;
   }
}

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
   parse_blockers(doc, out);
   cJSON_Delete(doc);
}

/* Repo-relative only: no absolute paths and no ".." path component (a citation
 * must point INSIDE the worktree under review). */
static int citation_path_safe(const char *rel)
{
   if (!rel || !rel[0] || rel[0] == '/')
      return 0;
   for (const char *p = rel; *p; p++)
   {
      if (p[0] != '.' || p[1] != '.')
         continue;
      int at_start = (p == rel) || (p[-1] == '/');
      int at_end = (p[2] == '\0') || (p[2] == '/');
      if (at_start && at_end)
         return 0;
   }
   return 1;
}

/* Does the blocker's citation reproduce in workdir/file? The file must exist
 * and the cited line must exist in it; when a quote is given, its text must
 * additionally appear within +/-2 lines of the cited line (the tolerance
 * forgives an off-by-a-couple citation — reviewers count from a diff). A
 * fabricated file, out-of-range line, or unmatched quote does not reproduce. */
static int citation_reproduces(const char *workdir, const wfe_blocker_t *b)
{
   char quote[sizeof b->quote];
   snprintf(quote, sizeof quote, "%s", b->quote);
   char *q = quote;
   while (*q == ' ' || *q == '\t')
      q++;
   for (char *p = q; *p; p++)
      if (*p == '\n' || *p == '\r')
      {
         *p = '\0'; /* compare the first cited line only */
         break;
      }
   size_t ql = strlen(q);
   while (ql > 0 && (q[ql - 1] == ' ' || q[ql - 1] == '\t'))
      q[--ql] = '\0';
   if (!citation_path_safe(b->file))
      return 0;

   char path[512];
   if (snprintf(path, sizeof path, "%s/%s", workdir, b->file) >= (int)sizeof path)
      return 0;
   FILE *fp = fopen(path, "r");
   if (!fp)
      return 0;
   char linebuf[4096];
   int lineno = 0, hit = 0;
   while (fgets(linebuf, sizeof linebuf, fp))
   {
      lineno++;
      if (ql == 0)
      {
         if (lineno >= b->line)
         {
            hit = 1; /* quote-less citation: the cited line exists */
            break;
         }
         continue;
      }
      if (lineno > b->line + 2)
         break;
      if (lineno >= b->line - 2 && strstr(linebuf, q))
      {
         hit = 1;
         break;
      }
   }
   fclose(fp);
   return hit;
}

int wfe_panel_blockers_verify(wfe_verdict_t *v, const char *workdir)
{
   if (!v || v->kind != WFE_V_REQUEST_CHANGES)
      return 0;
   if (!workdir || !workdir[0])
      return v->blocker_count; /* nothing to replay against -> keep, unverified
                                * (mirror INDEX_UNAVAILABLE: never penalize) */
   int verified = 0;
   for (int i = 0; i < v->blocker_count; i++)
   {
      v->blockers[i].verified = citation_reproduces(workdir, &v->blockers[i]);
      verified += v->blockers[i].verified;
   }
   if (verified > 0)
   {
      /* re-ground the blocking weight to what actually reproduced */
      v->high_sev_blockers = verified;
      return verified;
   }
   /* No citation reproduced (or none was given): an interpretive objection can
    * never block — re-grade to a non-blocking comment, exactly as the compute
    * roundtable caps NO_EVIDENCE items and rejects CONTRADICTED ones. */
   v->kind = WFE_V_COMMENT;
   v->high_sev_blockers = 0;
   size_t used = strlen(v->feedback);
   snprintf(v->feedback + used, sizeof v->feedback - used,
            "%s[panel-verify] request_changes re-graded to comment: no blocker citation "
            "(file:line + exact quote) reproduced in the worktree",
            used ? "\n" : "");
   return 0;
}
