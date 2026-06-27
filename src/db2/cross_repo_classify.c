/* cross_repo_classify.c: pure tier classification for the cross-repo resolver
 * (S2b): definition multiplicity (§3.5) + the deterministic gate/cap/producer
 * pipeline (§3.10). DB-free; see cross_repo_classify.h and
 * docs/proposals/pending/cross-repo-dependency-graph.md. */

#include "cross_repo_classify.h"

#include <string.h>

const char *xrepo_tier_name(xrepo_tier_t t)
{
   switch (t)
   {
   case XREPO_TIER_HIGH:
      return "high";
   case XREPO_TIER_MEDIUM:
      return "medium";
   case XREPO_TIER_LOW:
      return "low";
   case XREPO_TIER_AMBIGUOUS:
      return "ambiguous";
   case XREPO_TIER_UNIMPLEMENTED:
      return "unimplemented";
   default:
      return "none";
   }
}

/* ---- definition multiplicity (§3.5) -------------------------------------- */

static int streq(const char *a, const char *b)
{
   return a && b && strcmp(a, b) == 0;
}

/* Defs are "provably unrelated" only when two definer repos have KNOWN,
 * INCOMPATIBLE signatures: both arities known (>= 0) and differing, or both
 * parameter-type lists known (non-empty) and differing. Such a spread is a
 * genuine cross-repo name-clash and must not be rescued by the dominant-definer
 * check. Unknown signatures are the caller's signal for "not available": arity
 * < 0 (incl. variadic) and param_types "" are treated as not-provably-unrelated,
 * so compatible/variadic overloads are NOT forced to NAMECLASH -- the
 * dominant-definer hysteresis still decides. The defs[] contract (one row per
 * distinct definer repo, with arity/param_types or the unknown sentinels) is
 * the caller's (S3/S4) responsibility; see cross_repo_classify.h. */
static int provably_unrelated(const xrepo_def_t *defs, int n)
{
   for (int i = 0; i < n; i++)
      for (int j = i + 1; j < n; j++)
      {
         if (defs[i].arity >= 0 && defs[j].arity >= 0 && defs[i].arity != defs[j].arity)
            return 1;
         if (defs[i].param_types[0] && defs[j].param_types[0] &&
             !streq(defs[i].param_types, defs[j].param_types))
            return 1;
      }
   return 0;
}

xrepo_mult_t xrepo_classify_multiplicity(const xrepo_def_t *defs, const int *def_counts,
                                         const int *exporter, int n, const xrepo_mult_cfg_t *cfg)
{
   xrepo_mult_t r = {XREPO_MULT_SINGLE, n < 0 ? 0 : n, n == 1 ? 0 : -1};
   if (n <= 1)
      return r;

   /* Total definitions and the top-two definer repos by def count. Non-positive
    * def_counts are clamped to 0 so a malformed input can't make the share
    * arithmetic negative/ill-defined; total==0 -> no dominant (falls to NAMECLASH). */
   long total = 0;
   int top = 0, second = -1;
   for (int i = 0; i < n; i++)
   {
      int dc = def_counts[i] > 0 ? def_counts[i] : 0;
      total += dc;
      if (def_counts[i] > def_counts[top])
      {
         second = top;
         top = i;
      }
      else if (second < 0 || def_counts[i] > def_counts[second])
      {
         if (i != top)
            second = i;
      }
   }

   /* A genuinely divergent signature spread is always a name-clash. */
   if (provably_unrelated(defs, n))
   {
      r.kind = XREPO_MULT_NAMECLASH;
      return r;
   }

   /* Dominant-definer hysteresis (§3.5): top holds >= dom_share_pct% AND the
    * runner-up holds <= runnerup_share_pct% AND <= runnerup_abs defs AND no
    * non-dominant definer is itself a distinctive exporter. */
   int dom_share = cfg ? cfg->dom_share_pct : 90;
   int ru_share = cfg ? cfg->runnerup_share_pct : 5;
   int ru_abs = cfg ? cfg->runnerup_abs : 2;
   int top_pct = total > 0 ? (int)((def_counts[top] * 100L) / total) : 0;
   int second_pct = (second >= 0 && total > 0) ? (int)((def_counts[second] * 100L) / total) : 0;
   int second_cnt = second >= 0 ? def_counts[second] : 0;

   int other_exporter = 0;
   for (int i = 0; i < n; i++)
      if (i != top && exporter && exporter[i])
         other_exporter = 1;

   if (top_pct >= dom_share && second_pct <= ru_share && second_cnt <= ru_abs && !other_exporter)
   {
      r.kind = XREPO_MULT_DOMINANT;
      r.dominant_index = top;
      return r;
   }
   r.kind = XREPO_MULT_NAMECLASH;
   return r;
}

/* ---- the deterministic tier pipeline (§3.10) ----------------------------- */

/* One-tier downgrade, floored at LOW (caps never drop a passing edge below LOW;
 * gates handle exclusion/review separately). */
static xrepo_tier_t downgrade(xrepo_tier_t t)
{
   if (t == XREPO_TIER_HIGH)
      return XREPO_TIER_MEDIUM;
   if (t == XREPO_TIER_MEDIUM)
      return XREPO_TIER_LOW;
   return t;
}

static xrepo_tier_t producer_tier(xrepo_corrob_t c)
{
   switch (c)
   {
   case XREPO_CORROB_IMPORT:
   case XREPO_CORROB_TRUSTED_EXPORT:
      return XREPO_TIER_HIGH;
   case XREPO_CORROB_DOMINANT:
      return XREPO_TIER_MEDIUM;
   default:
      return XREPO_TIER_LOW;
   }
}

xrepo_classification_t xrepo_classify(const xrepo_candidate_t *c)
{
   xrepo_classification_t r;
   memset(&r, 0, sizeof(r));
   r.tier = XREPO_TIER_NONE;

   if (!c)
   {
      r.reason = "null";
      return r;
   }
   if (c->lang == XREPO_LANG_UNKNOWN)
   {
      r.tier = XREPO_TIER_UNIMPLEMENTED;
      r.reason = "unimplemented-language";
      return r;
   }

   int multi_definer = c->mult.kind == XREPO_MULT_NAMECLASH;
   int corroborated = c->corroboration != XREPO_CORROB_NONE;

   /* Gate 1 -- invariant (§3): S originates in the caller -> no external edge. */
   if (c->originated_in_caller)
   {
      r.reason = "invariant-originated-in-caller";
      return r;
   }
   /* Gate 2 -- distinctiveness (§3.3): fail -> excluded, or AMBIGUOUS if the
    * symbol is a multi-definer (surfaced for review, not silently dropped). */
   if (!c->distinctive)
   {
      if (multi_definer)
      {
         r.tier = XREPO_TIER_AMBIGUOUS;
         r.routed_to_review = 1;
         r.reason = "not-distinctive-multi-definer";
      }
      else
         r.reason = "not-distinctive";
      return r;
   }
   /* Gate 3 -- multiplicity (§3.5): a name-clash multi-definer without
    * corroboration is genuinely ambiguous -> review queue. */
   if (multi_definer && !corroborated)
   {
      r.tier = XREPO_TIER_AMBIGUOUS;
      r.routed_to_review = 1;
      r.reason = "nameclash-uncorroborated";
      return r;
   }
   /* Gate 4 -- dynamic imports (§3.7) are out of static tiering -> review. */
   if (c->modality == XREPO_IMPORT_DYNAMIC)
   {
      r.tier = XREPO_TIER_AMBIGUOUS;
      r.routed_to_review = 1;
      r.reason = "dynamic-import";
      return r;
   }

   /* Producer -- base tier from the corroboration route (§3.1). */
   r.base_tier = producer_tier(c->corroboration);
   xrepo_tier_t tier = r.base_tier;

   /* Caps (order-independent MIN; each lowers by at most one tier, §3.10). */
   if (c->caller_collision)
   {
      tier = downgrade(tier);
      r.caller_collision_applied = 1;
   }
   if (c->modality == XREPO_IMPORT_CONDITIONAL)
   {
      tier = downgrade(tier);
      r.modality_cap_applied = 1;
   }
   /* Trust cap (§0): an untrusted-rooted corroboration route cannot reach HIGH ->
    * cap at MEDIUM. The IMPORT route is rooted in the caller (untrusted caller
    * caps); the TRUSTED_EXPORT route is rooted in the definer (an untrusted
    * definer can never lend HIGH export). Both are guarded here so untrusted HIGH
    * cannot leak via either route. */
   int untrusted_import = !c->caller_trusted && c->corroboration == XREPO_CORROB_IMPORT;
   int untrusted_export = !c->definer_trusted && c->corroboration == XREPO_CORROB_TRUSTED_EXPORT;
   if ((untrusted_import || untrusted_export) && tier > XREPO_TIER_MEDIUM)
   {
      tier = XREPO_TIER_MEDIUM;
      r.trust_cap_applied = 1;
   }

   /* Structural invariant (§3.10): caps only lower, never raise -- the final
    * ladder tier never exceeds the producer's base tier. */
   if (tier > r.base_tier)
      tier = r.base_tier;

   r.tier = tier;
   r.reason = xrepo_tier_name(tier);
   return r;
}

/* ---- eviction score (§3.8) ----------------------------------------------- */

double xrepo_evidence_score(int distinctiveness_rank, int call_site_count, int corroboration_routes)
{
   /* Weighted so corroboration dominates, then call sites, then distinctiveness;
    * purely for deterministic lowest-evidence-first eviction, not a tier. */
   double d = distinctiveness_rank < 0 ? 0 : distinctiveness_rank;
   double s = call_site_count < 0 ? 0 : call_site_count;
   double r = corroboration_routes < 0 ? 0 : corroboration_routes;
   return 10.0 * r + 1.0 * s + 0.01 * d;
}
