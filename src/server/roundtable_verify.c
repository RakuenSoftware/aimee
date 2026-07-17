/* roundtable_verify.c: replay-verification pass over roundtable review items.
 * See headers/roundtable_verify.h. */
#include "roundtable_verify.h"

#include "dstr.h"

#include <stdlib.h>
#include <string.h>

/* Lower "blocking" to "suggestion"; leave "suggestion"/"nit"/other unchanged. */
static void cap_severity(const char *claimed, char *out, size_t cap)
{
   const char *v = (claimed && strcmp(claimed, "blocking") == 0) ? "suggestion"
                   : (claimed && claimed[0])                     ? claimed
                                                                 : "suggestion";
   snprintf(out, cap, "%s", v);
}

verify_action_t roundtable_grade_item(replay_status_t st, int factual, const char *claimed_sev,
                                      char *out_sev, size_t out_cap)
{
   if (out_sev && out_cap)
      out_sev[0] = '\0';
   switch (st)
   {
   case REPLAY_CONTRADICTED:
   case REPLAY_VACUOUS:
      return VERIFY_REJECT; /* claim does not reproduce / malformed query */
   case REPLAY_INDEX_UNAVAILABLE:
      /* cannot verify -> keep claimed severity, mark unverified (don't penalize) */
      snprintf(out_sev, out_cap, "%s", claimed_sev && claimed_sev[0] ? claimed_sev : "suggestion");
      return VERIFY_DEGRADE;
   case REPLAY_NO_EVIDENCE:
      /* interpretive item (no structured evidence): can never be "blocking" */
      cap_severity(claimed_sev, out_sev, out_cap);
      return VERIFY_CAP;
   case REPLAY_MATCH:
   case REPLAY_CORRECTED:
      if (factual)
      {
         /* reproduced factual trigger: the claimed severity stands (incl. blocking) */
         snprintf(out_sev, out_cap, "%s",
                  claimed_sev && claimed_sev[0] ? claimed_sev : "suggestion");
         return VERIFY_KEEP;
      }
      /* evidence reproduced but the panelist did not assert it as factual ->
       * treat as interpretive and cap (never escalate on interpretation). */
      cap_severity(claimed_sev, out_sev, out_cap);
      return VERIFY_CAP;
   }
   cap_severity(claimed_sev, out_sev, out_cap);
   return VERIFY_CAP;
}

/* Append src's source tokens to dst->sources (comma-separated), skipping any
 * already present, so a dedup merge preserves panelist attribution. */
static void merge_sources(roundtable_review_item_t *dst, const roundtable_review_item_t *src)
{
   if (!src->sources[0])
      return;
   char tmp[sizeof(src->sources)];
   snprintf(tmp, sizeof(tmp), "%s", src->sources);
   for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ","))
   {
      while (*tok == ' ')
         tok++;
      if (!tok[0] || strstr(dst->sources, tok))
         continue;
      size_t used = strlen(dst->sources);
      if (used + 2 < sizeof(dst->sources))
      {
         if (used)
         {
            dst->sources[used++] = ',';
            dst->sources[used++] = ' ';
            dst->sources[used] = '\0';
         }
         snprintf(dst->sources + used, sizeof(dst->sources) - used, "%s", tok);
      }
   }
}

/* Has an earlier kept item [0,upto) already got this non-empty idkey? */
static int idkey_seen(const roundtable_result_t *out, int upto, const char *idkey)
{
   if (!idkey || !idkey[0])
      return -1;
   for (int i = 0; i < upto; i++)
      if (strcmp(out->items[i].evidence.idkey, idkey) == 0)
         return i;
   return -1;
}

void roundtable_verify_items_with(roundtable_result_t *out, const replay_backend_t *be,
                                  int require_evidence)
{
   if (!out)
      return;
   /* be may be NULL: evidence_replay_with(NULL,...) returns INDEX_UNAVAILABLE, so
    * every item degrades (kept, unverified) — the correct "no index here" result. */
   out->rejected_count = 0;
   out->verified_count = 0;
   out->degraded_count = 0;
   out->capped_count = 0;

   int keep = 0;
   int n = out->item_count;
   if (n > ROUNDTABLE_MAX_REVIEW_ITEMS)
      n = ROUNDTABLE_MAX_REVIEW_ITEMS;

   for (int i = 0; i < n; i++)
   {
      roundtable_review_item_t it = out->items[i]; /* copy: we may compact in place */
      reduced_record_t rec;
      replay_status_t st = evidence_replay_with(be, &it.evidence, &rec);
      char sev[16];
      verify_action_t act =
          roundtable_grade_item(st, it.evidence.factual, it.severity, sev, sizeof(sev));

      /* Evidence gate: a finding the chair cannot check against the code (no structured
       * evidence) is an unfalsifiable opinion — reject it rather than keep it capped, so
       * the primary never has to triage a claim no one can verify or refute. */
      if (require_evidence && st == REPLAY_NO_EVIDENCE)
         act = VERIFY_REJECT;

      if (act == VERIFY_REJECT)
      {
         if (out->rejected_count < ROUNDTABLE_MAX_REVIEW_ITEMS)
         {
            out->rejected[out->rejected_count] = it;
            snprintf(out->rejected_reason[out->rejected_count],
                     sizeof(out->rejected_reason[out->rejected_count]), "%s",
                     replay_status_str(st));
            out->rejected_count++;
         }
         continue;
      }

      /* re-ground the item: final severity + the reproduced idkey (for dedup/audit) */
      snprintf(it.severity, sizeof(it.severity), "%s", sev);
      snprintf(it.evidence.idkey, sizeof(it.evidence.idkey), "%s", rec.idkey);

      /* post-replay dedup: two panelists who reproduced the same file:line set are
       * one finding (keep the earlier, fold the source count). */
      int dup = idkey_seen(out, keep, it.evidence.idkey);
      if (dup >= 0)
      {
         out->items[dup].count += it.count;
         merge_sources(&out->items[dup], &it); /* keep panelist attribution */
         continue;
      }

      if (act == VERIFY_DEGRADE)
         out->degraded_count++;
      else if (act == VERIFY_CAP)
         out->capped_count++;
      else
         out->verified_count++;

      out->items[keep++] = it;
   }

   out->item_count = keep;
}

void roundtable_verify_items(roundtable_result_t *out, int require_evidence)
{
   /* Use the process-registered backend (the server installs a kb_client-backed
    * one at startup). NULL backend => every item degrades (kept, unverified). The
    * verifier references no index/db2/kb symbol directly, so it links everywhere. */
   roundtable_verify_items_with(out, evidence_replay_active_backend(), require_evidence);
}

char *roundtable_render_rejected(const roundtable_result_t *out)
{
   if (!out)
      return NULL;
   if (out->rejected_count == 0 && out->degraded_count == 0 && out->capped_count == 0)
      return NULL;

   dstr_t s;
   dstr_init(&s);
   dstr_appendf(&s, "\n## Replay verification\n");
   dstr_appendf(&s, "verified=%d capped=%d degraded=%d rejected=%d\n", out->verified_count,
                out->capped_count, out->degraded_count, out->rejected_count);
   if (out->rejected_count > 0)
   {
      dstr_append_str(&s, "\n### Rejected at verification\n");
      for (int i = 0; i < out->rejected_count && i < ROUNDTABLE_MAX_REVIEW_ITEMS; i++)
      {
         const roundtable_review_item_t *it = &out->rejected[i];
         dstr_appendf(&s, "- **%s** (%s): %s — _%s_\n", it->category[0] ? it->category : "review",
                      it->location[0] ? it->location : "?", it->summary, out->rejected_reason[i]);
      }
   }
   return dstr_steal(&s);
}

void roundtable_artifact_append_rejected(char **artifact, const roundtable_result_t *out)
{
   if (!artifact || !*artifact)
      return;
   char *app = roundtable_render_rejected(out);
   if (!app)
      return;
   size_t la = strlen(*artifact), lb = strlen(app);
   char *merged = realloc(*artifact, la + lb + 1);
   if (merged)
   {
      memcpy(merged + la, app, lb + 1);
      *artifact = merged;
   }
   free(app);
}
