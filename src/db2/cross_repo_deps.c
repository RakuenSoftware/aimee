/* cross_repo_deps.c: S4a orchestration — generate cross-repo candidate edges and
 * resolve+classify them via the pure S2a/S2b core using the S3 stats. Emits
 * repo-level edges with evidence + version stamps. Postgres-backed; portable SQL
 * so the candidate path also runs on the sqlite shim (empty corpus -> 0 edges).
 * See cross_repo_deps.h and docs/proposals/pending/cross-repo-dependency-graph.md. */

#include "cross_repo_deps.h"

#include "aimee.h"
#include "config.h"
#include "cross_repo_review.h"
#include "cross_repo_stats.h"
#include "db2.h"
#include "db_postgres.h"
#include "log.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CRD_LOG_TAG     "cross_repo"
#define CRD_ERR         256
#define CRD_MAX_REPOS   256
#define CRD_MAX_HEADERS 20000
#define CRD_MAX_DEFS    64

/* Minimal JSON string escaper for embedding a symbol into the evidence blob:
 * escapes " \ and control bytes (< 0x20) as \uXXXX; truncates safely to cap. */
static void crd_json_escape(const char *in, char *out, size_t cap)
{
   size_t o = 0;
   if (cap == 0)
      return;
   for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 7 < cap; p++)
   {
      if (*p == '"' || *p == '\\')
      {
         out[o++] = '\\';
         out[o++] = (char)*p;
      }
      else if (*p < 0x20)
      {
         o += (size_t)snprintf(out + o, cap - o, "\\u%04x", *p);
      }
      else
         out[o++] = (char)*p;
   }
   out[o] = '\0';
}

/* ---- manifest module-id parsing (pure) ----------------------------------- */

static void trim_quotes_ws(char *s)
{
   size_t n = strlen(s);
   while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '"' || s[n - 1] == '\'' ||
                s[n - 1] == '\r' || s[n - 1] == ','))
      s[--n] = '\0';
   size_t i = 0;
   while (s[i] == ' ' || s[i] == '\t' || s[i] == '"' || s[i] == '\'')
      i++;
   if (i)
      memmove(s, s + i, strlen(s + i) + 1);
}

/* Find a `key`-prefixed value on its own line (toml/go.mod style: `key value` or
 * `key = value`). Returns 1 + fills out, else 0. */
static int line_value(const char *content, const char *key, char *out, size_t cap)
{
   size_t klen = strlen(key);
   const char *p = content;
   while (p && *p)
   {
      const char *eol = strchr(p, '\n');
      size_t llen = eol ? (size_t)(eol - p) : strlen(p);
      const char *q = p;
      while (*q == ' ' || *q == '\t')
         q++;
      if ((size_t)(p + llen - q) > klen && strncmp(q, key, klen) == 0 &&
          (q[klen] == ' ' || q[klen] == '\t' || q[klen] == '='))
      {
         const char *v = q + klen;
         while (*v == ' ' || *v == '\t' || *v == '=')
            v++;
         size_t vlen = (size_t)(p + llen - v);
         if (vlen >= cap)
            vlen = cap - 1;
         memcpy(out, v, vlen);
         out[vlen] = '\0';
         trim_quotes_ws(out);
         return out[0] ? 1 : 0;
      }
      if (!eol)
         break;
      p = eol + 1;
   }
   return 0;
}

/* Extract a JSON top-level "name": "value" (flat scan; sufficient for package.json). */
static int json_name(const char *content, char *out, size_t cap)
{
   const char *k = strstr(content, "\"name\"");
   if (!k)
      return 0;
   k = strchr(k + 6, ':');
   if (!k)
      return 0;
   k++;
   while (*k == ' ' || *k == '\t' || *k == '"')
      k++;
   size_t i = 0;
   while (k[i] && k[i] != '"' && i + 1 < cap)
      i++;
   if (i >= cap)
      i = cap - 1;
   memcpy(out, k, i);
   out[i] = '\0';
   return out[0] ? 1 : 0;
}

int xrepo_parse_module_id(const char *basename, const char *content, char *out, size_t cap)
{
   if (!basename || !content || !out || cap == 0)
      return 0;
   out[0] = '\0';
   if (strcmp(basename, "go.mod") == 0)
      return line_value(content, "module", out, cap);
   if (strcmp(basename, "package.json") == 0)
      return json_name(content, out, cap);
   if (strcmp(basename, "Cargo.toml") == 0 || strcmp(basename, "pyproject.toml") == 0)
      return line_value(content, "name", out, cap); /* [package]/[project] name = "x" */
   return 0;
}

/* ---- repo descriptors ---------------------------------------------------- */

typedef struct
{
   xrepo_repo_desc_t *d;
   size_t n;
   char *strpool;          /* not used; strings are individually freed */
   const char **hdr_store; /* flat backing for all headers[] arrays */
   size_t hdr_n;
} desc_set_t;

static const char *path_basename(const char *p)
{
   const char *s = strrchr(p, '/');
   return s ? s + 1 : p;
}

static void free_descs(desc_set_t *s)
{
   if (!s || !s->d)
      return;
   for (size_t i = 0; i < s->n; i++)
   {
      free((char *)s->d[i].name);
      free((char *)s->d[i].module_id);
   }
   for (size_t i = 0; i < s->hdr_n; i++)
      free((char *)s->hdr_store[i]);
   free(s->hdr_store);
   free(s->d);
   memset(s, 0, sizeof(*s));
}

/* Build a descriptor per registered repo: name, trust, module_id (from a manifest
 * in file_contents), and the set of indexed C/C++ header paths. */
static int load_descs(void *conn, desc_set_t *out)
{
   memset(out, 0, sizeof(*out));
   char err[CRD_ERR] = "";

   out->d = calloc(CRD_MAX_REPOS, sizeof(xrepo_repo_desc_t));
   out->hdr_store = calloc(CRD_MAX_HEADERS, sizeof(char *));
   if (!out->d || !out->hdr_store)
   {
      free_descs(out);
      return -1;
   }

   aimee_pg_stmt_t *st =
       aimee_pg_prepare(conn, "SELECT name, trust FROM projects ORDER BY name", err, sizeof(err));
   if (!st)
   {
      free_descs(out);
      return -1;
   }
   while (out->n < CRD_MAX_REPOS && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *name = aimee_pg_column_text(st, 0);
      const char *trust = aimee_pg_column_text(st, 1);
      xrepo_repo_desc_t *D = &out->d[out->n];
      D->name = strdup(name ? name : "");
      D->trusted = (trust && strcmp(trust, "trusted") == 0) ? 1 : 0;
      D->module_id = strdup("");
      D->headers = NULL;
      D->header_count = 0;
      out->n++;
   }
   aimee_pg_finalize(st);

   /* module_id: scan each repo's manifest content (best-effort, first match wins). */
   for (size_t i = 0; i < out->n; i++)
   {
      aimee_pg_stmt_t *m = aimee_pg_prepare(
          conn,
          "SELECT f.path, fc.content FROM file_contents fc JOIN files f ON f.id = fc.file_id "
          "JOIN projects p ON p.id = f.project_id WHERE p.name = ?1 AND "
          "(f.path LIKE '%go.mod' OR f.path LIKE '%Cargo.toml' OR f.path LIKE '%package.json' "
          "OR f.path LIKE '%pyproject.toml') ORDER BY length(f.path) LIMIT 8",
          err, sizeof(err));
      if (!m)
         continue;
      aimee_pg_bind_text(m, "?1", out->d[i].name);
      char id[256] = "";
      while (aimee_pg_step(m, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *path = aimee_pg_column_text(m, 0);
         const char *content = aimee_pg_column_text(m, 1);
         if (path && content && xrepo_parse_module_id(path_basename(path), content, id, sizeof(id)))
            break;
      }
      aimee_pg_finalize(m);
      if (id[0])
      {
         free((char *)out->d[i].module_id);
         out->d[i].module_id = strdup(id);
      }
   }

   /* headers: indexed C/C++ header paths per repo, sliced from the flat store. */
   for (size_t i = 0; i < out->n; i++)
   {
      size_t start = out->hdr_n;
      aimee_pg_stmt_t *h = aimee_pg_prepare(
          conn,
          "SELECT f.path FROM files f JOIN projects p ON p.id = f.project_id WHERE p.name = ?1 AND "
          "(f.path LIKE '%.h' OR f.path LIKE '%.hpp' OR f.path LIKE '%.hh' OR f.path LIKE '%.hxx') "
          "ORDER BY f.path",
          err, sizeof(err));
      if (!h)
         continue;
      aimee_pg_bind_text(h, "?1", out->d[i].name);
      while (out->hdr_n < CRD_MAX_HEADERS && aimee_pg_step(h, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *path = aimee_pg_column_text(h, 0);
         if (path && path[0])
            out->hdr_store[out->hdr_n++] = strdup(path);
      }
      aimee_pg_finalize(h);
      if (out->hdr_n > start)
      {
         out->d[i].headers = &out->hdr_store[start];
         out->d[i].header_count = out->hdr_n - start;
      }
   }
   return 0;
}

static int desc_index(const desc_set_t *s, const char *name)
{
   for (size_t i = 0; i < s->n; i++)
      if (strcmp(s->d[i].name, name) == 0)
         return (int)i;
   return -1;
}

/* ---- emitted-edge aggregation -------------------------------------------- */

typedef struct
{
   xrepo_dep_edge_t *e;
   size_t n, cap;
} edge_acc_t;

static xrepo_dep_edge_t *edge_find_or_add(edge_acc_t *a, const char *caller, const char *definer)
{
   for (size_t i = 0; i < a->n; i++)
      if (strcmp(a->e[i].caller_repo, caller) == 0 && strcmp(a->e[i].definer_repo, definer) == 0)
         return &a->e[i];
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 32;
      xrepo_dep_edge_t *ne = realloc(a->e, nc * sizeof(*ne));
      if (!ne)
         return NULL;
      a->e = ne;
      a->cap = nc;
   }
   xrepo_dep_edge_t *E = &a->e[a->n++];
   memset(E, 0, sizeof(*E));
   snprintf(E->caller_repo, sizeof(E->caller_repo), "%s", caller);
   snprintf(E->definer_repo, sizeof(E->definer_repo), "%s", definer);
   E->tier = XREPO_TIER_NONE;
   return E;
}

/* In-memory sink for AMBIGUOUS candidates under --dry-run (parallels edge_acc_t).
 * NULL cap means "not capturing" (normal operation routes to the review queue). */
typedef struct
{
   xrepo_amb_cand_t *c;
   size_t n, cap;
} amb_acc_t;

static int amb_push(amb_acc_t *a, const char *sym, const char *caller, const char *definer,
                    const char *ev, double score)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 16;
      xrepo_amb_cand_t *nn = realloc(a->c, nc * sizeof(*nn));
      if (!nn)
         return 0;
      a->c = nn;
      a->cap = nc;
   }
   xrepo_amb_cand_t *C = &a->c[a->n++];
   memset(C, 0, sizeof(*C));
   snprintf(C->symbol, sizeof(C->symbol), "%s", sym ? sym : "");
   snprintf(C->caller_repo, sizeof(C->caller_repo), "%s", caller ? caller : "");
   snprintf(C->candidate_definer, sizeof(C->candidate_definer), "%s", definer ? definer : "");
   snprintf(C->evidence, sizeof(C->evidence), "%s", ev ? ev : "");
   C->evidence_score = score;
   return 1;
}

/* Find an existing edge without adding (recall R2c merge). NULL if absent. */
static xrepo_dep_edge_t *edge_find(edge_acc_t *a, const char *caller, const char *definer)
{
   for (size_t i = 0; i < a->n; i++)
      if (strcmp(a->e[i].caller_repo, caller) == 0 && strcmp(a->e[i].definer_repo, definer) == 0)
         return &a->e[i];
   return NULL;
}

/* ---- orchestration ------------------------------------------------------- */

/* Shared, read-only context for the per-symbol flush. */
typedef struct
{
   const desc_set_t *descs;
   const char *imp_seen; /* CRD_MAX_REPOS flags: repos A imports (import route) */
   /* H1 structural-edge gate: CRD_MAX_REPOS flags, repos A has a precomputed
    * cross_repo_route to (import_module/import_header, H0d). A non-LOW edge
    * REQUIRES a route — a bare name match with no structural route is
    * LOW-unresolved and not emitted (precision-hardening §1). */
   const char *route_seen;
   const char *project;
   int caller_trusted;
   xrepo_distinct_cfg_t dcfg;
   xrepo_mult_cfg_t mcfg;
   int collision_c;
   xrepo_tier_t min_tier;
   const char *repo_set_hash;
   int distinctiveness_v;
   int64_t bsv;
   int a_filecount;
   int include_review; /* write AMBIGUOUS candidates to the review queue (S4b) */
   int queue_max;      /* review-queue overflow cap */
   int dry_run;        /* --dry-run: capture AMBIGUOUS in `amb`, write nothing */
   amb_acc_t *amb;     /* in-memory AMBIGUOUS sink when dry_run (else NULL) */
} crd_ctx_t;

/* Bounds-safe route_seen lookup by repo name (H1 structural-edge gate). desc_index
 * returns -1 (unknown name) or an index in [0, descs->n); descs->n is capped at
 * CRD_MAX_REPOS at load (load_descs), so a valid index is always < route_seen's
 * size. The explicit `< CRD_MAX_REPOS` bound documents that invariant and stays
 * safe if the load cap ever diverges from the array size. */
static int route_seen_has(const crd_ctx_t *ctx, const char *repo)
{
   int di = desc_index(ctx->descs, repo);
   /* Out-of-range / unknown name returns 0 = "no route" = FAIL-CLOSED: the gate
    * rejects (demotes to LOW), never permissively accepts. So a future load_descs
    * cap regression degrades recall (safe), never precision. */
   return (di >= 0 && di < CRD_MAX_REPOS) ? ctx->route_seen[di] : 0;
}

/* Classify one accumulated candidate symbol (its definer rows) and, if it clears
 * the gates/min_tier, fold it into the per-(caller,definer) edge accumulator.
 * defs[]/dcount[]/dexp[] hold the external definers (A excluded); `defs[i].repo`
 * strings are freed here. AMBIGUOUS routes to the review queue (S4b), not emitted. */
static void crd_flush(const crd_ctx_t *ctx, edge_acc_t *acc, const char *sym, int sites, int files,
                      const char *exfile, int exline, int callee_rc, int caller_files, int blocked,
                      xrepo_def_t *defs, int *dcount, int *dexp, int ndef, int originated)
{
   if (ndef == 0)
      return; /* no external definer */

   /* §4 vendor canonical-preference (precision-hardening §4a + §4b combined): a
    * symbol may be legitimately vendored (FetchContent subprojects/, header-only
    * stb/nlohmann in third_party/), so do NOT blanket-drop vendored definers. Drop
    * a vendored duplicate ONLY when a non-vendored definer of the SAME symbol is
    * ALSO structurally route-reachable from the caller — that canonical definer is
    * the real dependency, so the vendored copy is noise that would otherwise
    * inflate multiplicity into a false AMBIGUOUS / steal the target. If NO
    * non-vendored candidate is route-reachable, the vendored copy is KEPT: it may
    * be the copy the caller actually includes (caller vendors lib V's header), and
    * dropping it would lose the real caller->V edge (the H1 gate would then emit
    * nothing). A lone vendored definer is likewise kept (header-only-library
    * exemption); the H1 route gate still requires a real route to it.
    *
    * Runs before multiplicity/target so the canonical pass precedes AMBIGUOUS and
    * the review-queue upsert (pipeline order §7), so review rows are written over
    * the canonicalized candidate set (no stale vendored-candidate rows). Two-pass
    * for auditability: free dropped candidates' only heap field (.repo;
    * self_type/param_types are "" literals, dcount/dexp are parallel int arrays,
    * no heap), THEN compact survivors with no frees — the tail free loop covers
    * exactly the surviving [0,ndef). */
   int has_canonical_route = 0;
   for (int i = 0; i < ndef; i++)
      if (!defs[i].vendored && route_seen_has(ctx, defs[i].repo))
      {
         has_canonical_route = 1;
         break;
      }
   if (has_canonical_route)
   {
      for (int i = 0; i < ndef; i++)
         if (defs[i].vendored)
            free((char *)defs[i].repo);
      int w = 0;
      for (int i = 0; i < ndef; i++)
         if (!defs[i].vendored)
         {
            if (w != i)
            {
               defs[w] = defs[i];
               dcount[w] = dcount[i];
               dexp[w] = dexp[i];
            }
            w++;
         }
      ndef = w;
   }

   xrepo_lang_t lang = defs[0].lang;

   /* definer_repo_count over TRUSTED repos (incl. the caller if it is a trusted
    * definer) — the distinctiveness "defined in >= M repos" signal. Computed over
    * the CANONICALIZED candidate set (§4 may have dropped route-reachable-canonical
    * duplicates above), so it deliberately measures distinctiveness over canonical
    * definers, not raw physical copies — a symbol with one canonical definer plus N
    * vendored copies counts as 1, which is correct (the copies are the same symbol,
    * not evidence the name is generic). */
   int definer_rc = 0;
   for (int i = 0; i < ndef; i++)
   {
      int di = desc_index(ctx->descs, defs[i].repo);
      if (di >= 0 && ctx->descs->d[di].trusted)
         definer_rc++;
   }
   if (ctx->caller_trusted && originated)
      definer_rc++; /* the caller defines it too (A excluded from defs[]) */

   xrepo_distinct_stats_t stats = {
       .callee_repo_count = callee_rc,
       .definer_repo_count = definer_rc,
       .caller_file_pct = ctx->a_filecount > 0 ? (caller_files * 100) / ctx->a_filecount : 0};
   int distinctive = !blocked && xrepo_name_distinctive(sym, &stats, &ctx->dcfg);
   xrepo_mult_t mult = xrepo_classify_multiplicity(defs, dcount, dexp, ndef, &ctx->mcfg);

   /* Target definer + corroboration route. Import route (a definer A imports)
    * wins — repo-level per proposal §3.1a ("import names B AND S resolves to B":
    * the target is always a definer of S that A imports); else trusted-export on
    * the dominant definer; else dominant. */
   int target = -1;
   xrepo_corrob_t corr = XREPO_CORROB_NONE;
   for (int i = 0; i < ndef; i++)
   {
      int di = desc_index(ctx->descs, defs[i].repo);
      if (di >= 0 && ctx->imp_seen[di])
      {
         target = i;
         corr = XREPO_CORROB_IMPORT;
         break;
      }
   }
   if (target < 0 && mult.kind == XREPO_MULT_DOMINANT && mult.dominant_index >= 0)
   {
      target = mult.dominant_index;
      int di = desc_index(ctx->descs, defs[target].repo);
      int dt = di >= 0 ? ctx->descs->d[di].trusted : 0;
      corr = (dt && dexp[target] && sites >= 3 && files >= 3) ? XREPO_CORROB_TRUSTED_EXPORT
                                                              : XREPO_CORROB_DOMINANT;
   }

   int definer_trusted = 0;
   int target_has_route = 0;
   if (target >= 0)
   {
      int di = desc_index(ctx->descs, defs[target].repo);
      definer_trusted = di >= 0 ? ctx->descs->d[di].trusted : 0;
      target_has_route = route_seen_has(ctx, defs[target].repo);
   }

   /* H1 structural-edge gate, applied uniformly: does the caller have a precomputed
    * route to ANY of this symbol's definers? defs[0..ndef) IS the full external
    * definer set the resolver accumulated for this symbol (the same array target
    * selection and the AMBIGUOUS rep use), so this matches the resolver's definer
    * coverage exactly. If no route to any definer, the symbol is a bare name
    * collision with no structural backing — neither emit it (below) NOR surface it
    * to the review queue, so name-only noise can't leak in via the AMBIGUOUS path. */
   int any_route = 0;
   for (int i = 0; i < ndef && !any_route; i++)
      any_route = route_seen_has(ctx, defs[i].repo);

   xrepo_candidate_t c = {0};
   c.lang = lang;
   c.originated_in_caller = originated;
   c.distinctive = distinctive;
   c.mult = mult;
   c.caller_collision = files >= ctx->collision_c;
   c.corroboration = corr;
   c.caller_trusted = ctx->caller_trusted;
   c.definer_trusted = definer_trusted;
   c.modality = XREPO_IMPORT_STATIC;
   /* §5/§6/§4-ceiling inputs for the target definer (defaults are HIGH-permissive
    * when there is no target, so a no-target candidate is unaffected). */
   c.kind_macro_typedef = target >= 0 ? !defs[target].high_capable_kind : 0;
   c.exported = target >= 0 ? dexp[target] : 0;
   c.definer_vendored = target >= 0 ? defs[target].vendored : 0;
   xrepo_classification_t cl = xrepo_classify(&c);

   /* AMBIGUOUS -> review queue (§3.8), surfaced not dropped — but only when a
    * structural route exists to at least one definer (H1 invariant: no route =>
    * not a cross-repo relation at all, so not even review-worthy). The genuine
    * route-backed multi-definer collisions are H2's canonical pass. */
   if (cl.tier == XREPO_TIER_AMBIGUOUS && any_route && (ctx->include_review || ctx->dry_run))
   {
      int routes = (c.corroboration != XREPO_CORROB_NONE) ? 1 : 0;
      double score = xrepo_evidence_score(distinctive ? definer_rc : 0, sites, routes);
      /* Representative candidate_definer: deterministic across re-resolutions so
       * the fingerprint is stable (an adjudicated row is not orphaned by a tie
       * flip) — highest defcount, tie-broken by repo name. */
      int rep = 0;
      for (int i = 1; i < ndef; i++)
         if (dcount[i] > dcount[rep] ||
             (dcount[i] == dcount[rep] && strcmp(defs[i].repo, defs[rep].repo) < 0))
            rep = i;
      char esym[256];
      crd_json_escape(sym, esym, sizeof(esym));
      char ev[512];
      snprintf(ev, sizeof(ev),
               "{\"symbol\":\"%s\",\"definers\":%d,\"sites\":%d,\"files\":%d,\"reason\":\"%s\"}",
               esym, ndef, sites, files, cl.reason ? cl.reason : "ambiguous");
      /* --dry-run captures the candidate in-memory for offline inspection and
       * writes NOTHING; normal operation persists it to the review queue. */
      if (ctx->dry_run)
      {
         if (ctx->amb)
            amb_push(ctx->amb, sym, ctx->project, defs[rep].repo, ev, score);
      }
      else
         db2_cross_repo_review_upsert(ctx->repo_set_hash, sym, ctx->project, defs[rep].repo, ev,
                                      score, "ambiguous", 0, ctx->queue_max);
   }

   /* H1 structural-edge gate (precision-hardening §1): a non-LOW edge REQUIRES a
    * precomputed cross_repo_route caller->definer. A bare distinctive-name match
    * with no real import/module route is LOW-unresolved and not emitted — this is
    * what collapses the name-collision false positives (DEFINE_GUID, generic
    * exports) that have no structural route to the named definer. */
   /* Instrumentation for H4 recall analysis: a candidate that WOULD have emitted a
    * non-LOW edge but is held back solely for lack of a structural route. Lets H4
    * distinguish no-route demotions (this) from route-but-untrusted-definer drops,
    * so link-only recall loss (build/link routes H0d doesn't model yet) is
    * measurable before deciding on H0e link-directive extraction. */
   if (target >= 0 && !target_has_route && cl.tier >= ctx->min_tier &&
       cl.tier != XREPO_TIER_AMBIGUOUS && cl.tier != XREPO_TIER_NONE &&
       cl.tier != XREPO_TIER_UNIMPLEMENTED)
      LOG_DEBUG(CRD_LOG_TAG, "low-unresolved (no route): %s -> %s via %s", ctx->project,
                defs[target].repo, sym);

   if (target >= 0 && target_has_route && cl.tier >= ctx->min_tier &&
       cl.tier != XREPO_TIER_AMBIGUOUS && cl.tier != XREPO_TIER_NONE &&
       cl.tier != XREPO_TIER_UNIMPLEMENTED)
   {
      xrepo_dep_edge_t *E = edge_find_or_add(acc, ctx->project, defs[target].repo);
      if (E)
      {
         if (cl.tier > E->tier)
            E->tier = cl.tier; /* repo edge = best linking-symbol tier */
         E->symbol_count++;
         E->call_site_count += sites;
         if (corr == XREPO_CORROB_IMPORT)
            E->import_corroborated = 1;
         if (corr == XREPO_CORROB_TRUSTED_EXPORT)
            E->export_corroborated = 1;
         if (!E->example_symbol[0])
         {
            snprintf(E->example_symbol, sizeof(E->example_symbol), "%s", sym);
            snprintf(E->example_file, sizeof(E->example_file), "%s", exfile);
            E->example_line = exline;
         }
         snprintf(E->repo_set_hash, sizeof(E->repo_set_hash), "%s", ctx->repo_set_hash);
         E->distinctiveness_v = ctx->distinctiveness_v;
         E->blocked_symbols_version = ctx->bsv;
         E->resolver_version = XREPO_RESOLVER_VERSION;
      }
   }
   for (int i = 0; i < ndef; i++)
      free((char *)defs[i].repo);
}

/* OUT-direction resolver: emit the cross-repo edges `project` -> D (deps OF
 * `project`). This is the core engine; direction=IN/BOTH is layered on top by the
 * public canonical_index_cross_repo_deps dispatcher, which reuses this per caller.
 * opts->direction is IGNORED here (always computes OUT for the given project). */
static int crd_compute_out(const char *project, const xrepo_deps_opts_t *opts,
                           xrepo_dep_edge_t **out_edges, size_t *out_n, int *truncated,
                           amb_acc_t *amb)
{
   if (out_edges)
      *out_edges = NULL;
   if (out_n)
      *out_n = 0;
   if (truncated)
      *truncated = 0;
   void *conn = db2_conn();
   if (!conn || !project || !opts || !out_edges || !out_n)
      return -1;

   config_t cfg;
   config_load(&cfg);
   if (!cfg.kb_curator_cross_repo_graph_enabled)
      return 0; /* feature off: empty result, not an error */

   xrepo_distinct_cfg_t dcfg = {.k = cfg.kb_curator_cross_repo_k,
                                .m = cfg.kb_curator_cross_repo_m,
                                .p_pct = cfg.kb_curator_cross_repo_p_pct,
                                .len_min = cfg.kb_curator_cross_repo_len_min};
   xrepo_mult_cfg_t mcfg = {.dom_share_pct = 90, .runnerup_share_pct = 5, .runnerup_abs = 2};
   int cap =
       opts->max_candidates > 0 ? opts->max_candidates : cfg.kb_curator_cross_repo_max_candidates;
   /* --dry-run emits every confidence band (down to LOW) for offline inspection;
    * otherwise honor the requested min_tier (default MEDIUM). */
   xrepo_tier_t min_tier =
       opts->dry_run ? XREPO_TIER_LOW : (opts->min_tier ? opts->min_tier : XREPO_TIER_MEDIUM);
   int collision_c = cfg.kb_curator_cross_repo_caller_collision_c;

   desc_set_t descs;
   if (load_descs(conn, &descs) != 0)
      return -1;
   int a_idx = desc_index(&descs, project);

   char repo_set_hash[24] = "";
   db2_cross_repo_repo_set_hash(repo_set_hash, sizeof(repo_set_hash));
   int64_t bsv = 0;
   db2_cross_repo_meta_read(NULL, &bsv, NULL, 0);

   /* repos A imports (resolved to a single repo) — the import-route set. */
   char imp_seen[CRD_MAX_REPOS];
   memset(imp_seen, 0, sizeof(imp_seen));
   if (a_idx >= 0)
   {
      char err[CRD_ERR] = "";
      aimee_pg_stmt_t *im = aimee_pg_prepare(
          conn,
          "SELECT DISTINCT i.name, f.path FROM file_imports i JOIN files f ON f.id = i.file_id "
          "JOIN projects p ON p.id = f.project_id WHERE p.name = ?1",
          err, sizeof(err));
      if (im)
      {
         aimee_pg_bind_text(im, "?1", project);
         while (aimee_pg_step(im, err, sizeof(err)) == AIMEE_PG_ROW)
         {
            const char *raw = aimee_pg_column_text(im, 0);
            const char *fp = aimee_pg_column_text(im, 1);
            xrepo_lang_t lang = xrepo_lang_from_path(fp ? fp : "");
            xrepo_resolve_result_t r = xrepo_resolve_import_to_repo(
                raw, lang, project, XREPO_IMPORT_STATIC, descs.d, descs.n);
            if (r.cardinality == XREPO_RESOLVE_ONE && r.repo_index >= 0 &&
                r.repo_index < (int)descs.n)
               imp_seen[r.repo_index] = 1;
         }
         aimee_pg_finalize(im);
      }
   }

   edge_acc_t acc = {0};
   char err[CRD_ERR] = "";

   /* A's file count (caller_file_pct denominator), fetched once. */
   int a_filecount = 0;
   {
      aimee_pg_stmt_t *fc = aimee_pg_prepare(
          conn,
          "SELECT COUNT(*) FROM files f JOIN projects p ON p.id = f.project_id WHERE p.name = ?1",
          err, sizeof(err));
      if (fc)
      {
         aimee_pg_bind_text(fc, "?1", project);
         if (aimee_pg_step(fc, err, sizeof(err)) == AIMEE_PG_ROW)
            a_filecount = aimee_pg_column_int(fc, 0);
         aimee_pg_finalize(fc);
      }
   }

   /* H1 structural-edge gate: repos A has a precomputed cross_repo_route to (H0d).
    * Read the inter-repo route adjacency once (off the per-candidate path) so the
    * flush can require a structural route in-memory without an N+1 query.
    * Concurrency: db2_cross_repo_rebuild_routes rebuilds the table inside a single
    * BEGIN/DELETE/INSERT/COMMIT txn, so under Postgres MVCC this one SELECT sees a
    * complete pre- or post-rebuild snapshot — never a half-rebuilt table. A rebuild
    * committing between this load and the later candidate queries can at worst use
    * one-call-stale routes (self-healing next call); the rebuild's atomicity rules
    * out the partial-state precision leak. Keyed by projects.name on BOTH sides
    * (rebuild_routes writes caller/definer_project from projects.name; desc_index
    * resolves the same column) so there is no writer/reader name-form drift. */
   char route_seen[CRD_MAX_REPOS];
   memset(route_seen, 0, sizeof(route_seen));
   if (a_idx >= 0)
   {
      char rerr[CRD_ERR] = "";
      aimee_pg_stmt_t *rt = aimee_pg_prepare(
          conn, "SELECT DISTINCT definer_project FROM cross_repo_route WHERE caller_project = ?1",
          rerr, sizeof(rerr));
      if (rt)
      {
         aimee_pg_bind_text(rt, "?1", project);
         while (aimee_pg_step(rt, rerr, sizeof(rerr)) == AIMEE_PG_ROW)
         {
            const char *dn = aimee_pg_column_text(rt, 0);
            int di = dn ? desc_index(&descs, dn) : -1;
            if (di >= 0 && di < CRD_MAX_REPOS) /* bound: descs.n <= CRD_MAX_REPOS */
               route_seen[di] = 1;
         }
         aimee_pg_finalize(rt);
      }
   }

   crd_ctx_t ctx = {.descs = &descs,
                    .imp_seen = imp_seen,
                    .route_seen = route_seen,
                    .project = project,
                    .caller_trusted = (a_idx >= 0) ? descs.d[a_idx].trusted : 1,
                    .dcfg = dcfg,
                    .mcfg = mcfg,
                    .collision_c = collision_c,
                    .min_tier = min_tier,
                    .repo_set_hash = repo_set_hash,
                    .distinctiveness_v = cfg.kb_curator_cross_repo_distinctiveness_v,
                    .bsv = bsv,
                    .a_filecount = a_filecount,
                    .include_review = opts->dry_run ? 0 : opts->include_review,
                    .queue_max = cfg.kb_curator_cross_repo_review_queue_max,
                    .dry_run = opts->dry_run,
                    .amb = opts->dry_run ? amb : NULL};

   /* Single working-set query (no per-candidate N+1, per the S3 contract): one row
    * per (candidate symbol, definer repo) with per-symbol stats as correlated
    * subqueries, ordered by symbol then definer-count. Rows for the same symbol are
    * contiguous; we accumulate them and flush per symbol, capping DISTINCT symbols
    * (cap+1 distinct -> truncated). */
   aimee_pg_stmt_t *cq = aimee_pg_prepare(
       conn,
       "WITH cand AS ("
       "  SELECT cc.callee AS sym, COUNT(*) AS sites, COUNT(DISTINCT cc.file_id) AS files, "
       "         MIN(f.path) AS exfile, MIN(cc.line) AS exline "
       "  FROM code_calls cc JOIN files f ON f.id = cc.file_id "
       "  JOIN projects p ON p.id = f.project_id WHERE p.name = ?1 GROUP BY cc.callee) "
       "SELECT c.sym, c.sites, c.files, c.exfile, c.exline, dp.name AS definer, "
       "       COUNT(*) AS defcount, dp.trust AS dtrust, "
       "       (SELECT COUNT(*) FROM file_exports e JOIN files fe ON fe.id = e.file_id "
       "          JOIN projects pe ON pe.id = fe.project_id WHERE pe.name = dp.name "
       "          AND e.name = c.sym) AS dexp, "
       "       (SELECT COUNT(DISTINCT p2.id) FROM code_calls cc2 JOIN files f2 ON f2.id = "
       "cc2.file_id "
       "          JOIN projects p2 ON p2.id = f2.project_id WHERE cc2.callee = c.sym "
       "          AND p2.trust = 'trusted') AS callee_rc, "
       "       (SELECT COUNT(DISTINCT fa.id) FROM code_calls cca JOIN files fa ON fa.id = "
       "cca.file_id "
       "          JOIN projects pa ON pa.id = fa.project_id WHERE pa.name = ?1 "
       "          AND cca.callee = c.sym) AS caller_files, "
       "       (SELECT COUNT(*) FROM blocked_symbols b WHERE b.word = c.sym AND b.lang = '') AS "
       "blk, "
       "       MIN(df.vendored) AS dvendored, "
       "       MAX(CASE WHEN dt.def_kind IN ('macro','typedef') THEN 0 ELSE 1 END) AS high_capable "
       "FROM cand c JOIN terms dt ON dt.name = c.sym AND dt.kind = 'definition' "
       "JOIN files df ON df.id = dt.file_id JOIN projects dp ON dp.id = df.project_id "
       "GROUP BY c.sym, c.sites, c.files, c.exfile, c.exline, dp.name, dp.trust "
       "ORDER BY c.sym, defcount DESC",
       err, sizeof(err));
   if (!cq)
   {
      free_descs(&descs);
      return -1;
   }
   aimee_pg_bind_text(cq, "?1", project);
   int seen = 0;

   /* Rows are contiguous per symbol (ORDER BY sym); accumulate a symbol's definer
    * rows then flush. Cap by DISTINCT symbol (the (cap+1)th distinct -> truncated). */
   char cur[128] = "";
   int have = 0, csites = 0, cfiles = 0, cexline = 0, ccallee = 0, ccfiles = 0, cblk = 0, corig = 0;
   char cexfile[MAX_PATH_LEN] = "";
   xrepo_def_t defs[CRD_MAX_DEFS];
   int dcount[CRD_MAX_DEFS], dexp[CRD_MAX_DEFS], ndef = 0;

   while (aimee_pg_step(cq, err, sizeof(err)) == AIMEE_PG_ROW)
   {
      const char *sym = aimee_pg_column_text(cq, 0);
      if (!sym)
         sym = "";
      if (have && strcmp(sym, cur) != 0)
      {
         crd_flush(&ctx, &acc, cur, csites, cfiles, cexfile, cexline, ccallee, ccfiles, cblk, defs,
                   dcount, dexp, ndef, corig);
         ndef = 0;
         have = 0;
      }
      if (!have)
      {
         if (seen >= cap)
         {
            if (truncated)
               *truncated = 1;
            break; /* don't start a new (capped) symbol */
         }
         seen++;
         have = 1;
         corig = 0;
         snprintf(cur, sizeof(cur), "%s", sym);
         csites = aimee_pg_column_int(cq, 1);
         cfiles = aimee_pg_column_int(cq, 2);
         const char *ef = aimee_pg_column_text(cq, 3);
         snprintf(cexfile, sizeof(cexfile), "%s", ef ? ef : "");
         cexline = aimee_pg_column_int(cq, 4);
         ccallee = aimee_pg_column_int(cq, 9);
         ccfiles = aimee_pg_column_int(cq, 10);
         cblk = aimee_pg_column_int(cq, 11) > 0 ? 1 : 0;
      }
      /* accumulate this definer row (A excluded from defs[]; sets originated). */
      const char *definer = aimee_pg_column_text(cq, 5);
      if (definer && strcmp(definer, project) == 0)
      {
         /* §3 (R3a): A "originates" S only if it has a NON-vendored definition. Col 12
          * is the per-(symbol, definer) MIN(df.vendored) from the working-set SELECT
          * (same positional column the defs[] accumulation below reads); over the 0/1
          * files.vendored flag (NOT NULL DEFAULT 0; the dt JOIN df guarantees >=1 row
          * so MIN is never NULL), MIN==1 iff EVERY one of A's defs of S is vendored.
          * In that case A's only copy is a dep fetched into _deps/, a vendor/ tree,
          * etc. — the DEP's code, not A's own — so it must NOT suppress the cross-repo
          * edge as originated; the symbol path resolves A's use to the canonical
          * (non-vendored) definer. MIN==0 (a genuine original def) still originates. */
         if (aimee_pg_column_int(cq, 12) == 0)
            corig = 1;
      }
      else if (definer && ndef < CRD_MAX_DEFS)
      {
         defs[ndef].repo = strdup(definer);
         defs[ndef].lang = xrepo_lang_from_path(cexfile);
         defs[ndef].self_type = "";
         defs[ndef].arity = -1;
         defs[ndef].param_types = "";
         /* §4 MIN(df.vendored) at column 12, §5 high_capable at column 13 — the last
          * SELECT columns. Adding query columns must APPEND + bump these indices;
          * this is the sole consumer of the cursor. */
         defs[ndef].vendored = aimee_pg_column_int(cq, 12) > 0 ? 1 : 0;
         defs[ndef].high_capable_kind = aimee_pg_column_int(cq, 13) > 0 ? 1 : 0;
         dcount[ndef] = aimee_pg_column_int(cq, 6);
         dexp[ndef] = aimee_pg_column_int(cq, 8) > 0 ? 1 : 0;
         ndef++;
      }
   }
   if (have)
      crd_flush(&ctx, &acc, cur, csites, cfiles, cexfile, cexline, ccallee, ccfiles, cblk, defs,
                dcount, dexp, ndef, corig);
   aimee_pg_finalize(cq);
   free_descs(&descs);

   /* R2c: merge build-DECLARED deps (recall-recovery §2.6) as a SEPARATE evidence
    * class — read cross_repo_build_dep for this caller (OUT direction) and fold into
    * the same (caller, definer) output. NOT via the H1 symbol-route gate (a build
    * declaration is its own evidence). Merge table: symbol+build(same definer) ->
    * "both", HIGH if the build dep is high-parse; build-only -> MEDIUM (high-parse) /
    * LOW (low-parse), subject to min_tier. A pair with multiple build declarations
    * takes the strongest tier. */
   {
      char berr[CRD_ERR] = "";
      aimee_pg_stmt_t *bd = aimee_pg_prepare(
          conn,
          "SELECT definer_project, build_kind, parse_confidence FROM cross_repo_build_dep "
          "WHERE caller_project = ?1 "
          "ORDER BY definer_project, (parse_confidence = 'high') DESC, build_kind",
          berr, sizeof(berr));
      if (bd)
      {
         aimee_pg_bind_text(bd, "?1", project);
         while (aimee_pg_step(bd, berr, sizeof(berr)) == AIMEE_PG_ROW)
         {
            const char *def = aimee_pg_column_text(bd, 0);
            const char *bkind = aimee_pg_column_text(bd, 1);
            const char *pconf = aimee_pg_column_text(bd, 2);
            if (!def || !def[0])
               continue;
            int high = pconf && strcmp(pconf, "high") == 0;
            xrepo_tier_t bt = high ? XREPO_TIER_MEDIUM : XREPO_TIER_LOW;
            xrepo_dep_edge_t *E = edge_find(&acc, project, def);
            if (E)
            {
               /* An edge already exists for this pair. It carries symbol evidence iff
                * its evidence_type is still pristine (untouched symbol edge) or already
                * "both"; "build_declared" means a prior build row created it with no
                * symbol corroboration. Promotion must be ORDER-INDEPENDENT: a high-parse
                * build row promotes a symbol-bearing pair to HIGH whether it is seen
                * before or after a low-parse row for the same pair (§2.6). */
               int has_symbol = (E->evidence_type[0] == 0) || strcmp(E->evidence_type, "both") == 0;
               if (has_symbol)
               {
                  /* symbol + build -> both; high-parse promotes to HIGH. build_kind takes
                   * the first row in the ORDER BY (high-parse first, then build_kind), i.e.
                   * the highest-parse declaration's kind — deterministic provenance for the
                   * representative build edge. */
                  snprintf(E->evidence_type, sizeof(E->evidence_type), "both");
                  if (!E->build_kind[0] && bkind)
                     snprintf(E->build_kind, sizeof(E->build_kind), "%s", bkind);
                  if (high && E->tier < XREPO_TIER_HIGH)
                     E->tier = XREPO_TIER_HIGH;
               }
               else
               {
                  /* another build declaration for an already-build-only edge -> keep the
                   * strongest tier. */
                  if (bt > E->tier && bt >= ctx.min_tier)
                     E->tier = bt;
               }
            }
            else if (bt >= ctx.min_tier)
            {
               xrepo_dep_edge_t *N = edge_find_or_add(&acc, project, def);
               if (N && N->tier == XREPO_TIER_NONE)
               {
                  N->tier = bt;
                  snprintf(N->evidence_type, sizeof(N->evidence_type), "build_declared");
                  if (bkind)
                     snprintf(N->build_kind, sizeof(N->build_kind), "%s", bkind);
                  snprintf(N->repo_set_hash, sizeof(N->repo_set_hash), "%s", repo_set_hash);
                  N->resolver_version = XREPO_RESOLVER_VERSION;
               }
            }
         }
         aimee_pg_finalize(bd);
      }
   }
   /* default evidence_type for pure symbol-resolved edges (untouched by the merge). */
   for (size_t i = 0; i < acc.n; i++)
      if (!acc.e[i].evidence_type[0])
         snprintf(acc.e[i].evidence_type, sizeof(acc.e[i].evidence_type), "symbol_resolved");

   /* Reverse-of-build suppression (precision): drop a PURE symbol-resolved edge
    * project->D when D BUILD-DECLARES project (a build dep D->project exists). The
    * build graph is authoritative for dependency DIRECTION — a symbol edge in the
    * reverse direction is a name-collision artifact (e.g. inputtino->wolf via a
    * `create_touch_screen` collision when wolf->inputtino is the real FetchContent
    * dep). Edges carrying their OWN forward build evidence (evidence_type "both" or
    * "build_declared") are never suppressed, so a genuine mutual/cyclic build
    * dependency is preserved. */
   {
      char rerr[CRD_ERR] = "";
      /* parse_confidence <> 'low': only a CONFIDENT build declaration is authoritative
       * for direction — a low-parse (guessed ${VAR}/conditional) build claim must not
       * suppress a real symbol edge. */
      aimee_pg_stmt_t *rb =
          aimee_pg_prepare(conn,
                           "SELECT DISTINCT caller_project FROM cross_repo_build_dep "
                           "WHERE definer_project = ?1 AND parse_confidence <> 'low'",
                           rerr, sizeof(rerr));
      if (!rb)
         LOG_WARN(CRD_LOG_TAG, "reverse-of-build suppression prepare failed: %s", rerr);
      if (rb)
      {
         aimee_pg_bind_text(rb, "?1", project);
         char rev[CRD_MAX_DEFS][128];
         int nrev = 0;
         while (aimee_pg_step(rb, rerr, sizeof(rerr)) == AIMEE_PG_ROW)
         {
            const char *c = aimee_pg_column_text(rb, 0);
            if (!c || !c[0])
               continue;
            if (nrev >= CRD_MAX_DEFS)
            {
               LOG_WARN(CRD_LOG_TAG,
                        "reverse-of-build set for '%s' exceeds %d; suppression partial", project,
                        CRD_MAX_DEFS);
               break;
            }
            snprintf(rev[nrev++], sizeof(rev[0]), "%s", c);
         }
         aimee_pg_finalize(rb);
         if (nrev > 0)
         {
            size_t w = 0;
            for (size_t i = 0; i < acc.n; i++)
            {
               int drop = 0;
               if (strcmp(acc.e[i].evidence_type, "symbol_resolved") == 0)
                  for (int j = 0; j < nrev; j++)
                     if (strcmp(acc.e[i].definer_repo, rev[j]) == 0)
                     {
                        drop = 1;
                        break;
                     }
               if (!drop)
                  acc.e[w++] = acc.e[i];
            }
            acc.n = w;
         }
      }
   }

   *out_edges = acc.e;
   *out_n = acc.n;
   return 0;
}

/* Append a full copy of one edge to an accumulator (used by the IN/BOTH merge;
 * unlike edge_find_or_add it never merges — each reverse caller yields at most one
 * edge to the target so duplicates cannot arise). Returns 0 on OOM. */
static int agg_push(edge_acc_t *a, const xrepo_dep_edge_t *src)
{
   if (a->n == a->cap)
   {
      size_t nc = a->cap ? a->cap * 2 : 32;
      xrepo_dep_edge_t *ne = realloc(a->e, nc * sizeof(*ne));
      if (!ne)
         return 0;
      a->e = ne;
      a->cap = nc;
   }
   a->e[a->n++] = *src;
   return 1;
}

/* IN-direction resolver: emit the cross-repo edges A -> `target` (repos that
 * depend ON `target`). Reuses crd_compute_out per candidate caller so a reverse
 * edge is BYTE-IDENTICAL to the forward edge the OUT query would emit (symmetric
 * consistency), inheriting every precision/recall/suppression rule. Candidate
 * callers = the union of repos with a precomputed structural route into `target`
 * (cross_repo_route) and repos that build-declare `target` (cross_repo_build_dep);
 * this is a superset of the emitters, kept small so the per-caller fan-out stays
 * bounded (§4.2). Edges are collected up to the candidate cap (then *trunc=1). */
static int crd_compute_in(const char *target, const xrepo_deps_opts_t *opts, edge_acc_t *agg,
                          int *trunc)
{
   void *conn = db2_conn();
   if (!conn || !target || !opts || !agg || !trunc)
      return -1;

   char callers[CRD_MAX_REPOS][128];
   int nc = 0;
   static const char *const caller_sql[2] = {
       "SELECT DISTINCT caller_project FROM cross_repo_route WHERE definer_project = ?1",
       "SELECT DISTINCT caller_project FROM cross_repo_build_dep WHERE definer_project = ?1"};
   for (int q = 0; q < 2 && nc < CRD_MAX_REPOS; q++)
   {
      char err[CRD_ERR] = "";
      aimee_pg_stmt_t *st = aimee_pg_prepare(conn, caller_sql[q], err, sizeof(err));
      if (!st)
         continue;
      aimee_pg_bind_text(st, "?1", target);
      while (nc < CRD_MAX_REPOS && aimee_pg_step(st, err, sizeof(err)) == AIMEE_PG_ROW)
      {
         const char *c = aimee_pg_column_text(st, 0);
         if (!c || !c[0] || strcmp(c, target) == 0) /* a repo is never its own dependent */
            continue;
         int dup = 0;
         for (int i = 0; i < nc; i++)
            if (strcmp(callers[i], c) == 0)
            {
               dup = 1;
               break;
            }
         if (!dup)
            snprintf(callers[nc++], sizeof(callers[0]), "%s", c);
      }
      aimee_pg_finalize(st);
   }

   config_t cfg;
   config_load(&cfg);
   int cap =
       opts->max_candidates > 0 ? opts->max_candidates : cfg.kb_curator_cross_repo_max_candidates;

   /* Per-caller OUT, keeping only edges whose definer IS the target. Force the OUT
    * direction and suppress review-queue writes: a reverse query is a read, and its
    * per-caller fan-out must not enqueue AMBIGUOUS candidates for other repos. */
   xrepo_deps_opts_t sub = *opts;
   sub.direction = XREPO_DIR_OUT;
   sub.include_review = 0;
   for (int i = 0; i < nc; i++)
   {
      if (cap > 0 && (int)agg->n >= cap)
      {
         *trunc = 1;
         break; /* candidate cap reached — stop the per-caller fan-out early. */
      }
      xrepo_dep_edge_t *e = NULL;
      size_t n = 0;
      int t = 0;
      if (crd_compute_out(callers[i], &sub, &e, &n, &t, NULL) != 0)
      {
         /* A per-caller failure (transient DB/OOM) must not be silently swallowed:
          * flag the result partial (§4.2) so a reverse read cannot masquerade as a
          * complete "no dependents" answer when a caller was actually skipped. */
         *trunc = 1;
         continue;
      }
      if (t)
         *trunc = 1;
      for (size_t j = 0; j < n; j++)
      {
         if (strcmp(e[j].definer_repo, target) != 0)
            continue;
         if (cap > 0 && (int)agg->n >= cap)
         {
            *trunc = 1;
            break;
         }
         if (!agg_push(agg, &e[j]))
         {
            free(e);
            return -1;
         }
      }
      free(e);
   }
   return 0;
}

/* Public entry (extended): dispatch on direction and, under --dry-run, also return
 * the AMBIGUOUS candidates captured in-memory. OUT delegates to the core engine; IN
 * and BOTH layer the reverse traversal (crd_compute_in) on top, reusing OUT per
 * caller for symmetric consistency (§B --reverse, §4 direction=in|both). Ambiguous
 * capture applies to the forward (OUT) computation only. */
int canonical_index_cross_repo_deps_ex(const char *project, const xrepo_deps_opts_t *opts,
                                       xrepo_dep_edge_t **out_edges, size_t *out_n, int *truncated,
                                       xrepo_amb_cand_t **out_amb, size_t *out_amb_n)
{
   if (out_edges)
      *out_edges = NULL;
   if (out_n)
      *out_n = 0;
   if (truncated)
      *truncated = 0;
   if (out_amb)
      *out_amb = NULL;
   if (out_amb_n)
      *out_amb_n = 0;
   if (!project || !opts || !out_edges || !out_n)
      return -1;

   /* Capture AMBIGUOUS candidates in-memory only when the caller wants them and
    * we are in dry-run (forward computation). */
   amb_acc_t amb = {0};
   amb_acc_t *ambp = (out_amb && opts->dry_run) ? &amb : NULL;

   int rc;
   if (opts->direction == XREPO_DIR_OUT)
   {
      rc = crd_compute_out(project, opts, out_edges, out_n, truncated, ambp);
   }
   else
   {
      /* IN or BOTH: build a fresh accumulator. For BOTH, seed it with the forward
       * (OUT) edges (capturing ambiguous), then append the reverse (IN) edges. */
      edge_acc_t agg = {0};
      int trunc = 0;
      rc = 0;
      if (opts->direction == XREPO_DIR_BOTH)
      {
         xrepo_dep_edge_t *oe = NULL;
         size_t on = 0;
         int ot = 0;
         if (crd_compute_out(project, opts, &oe, &on, &ot, ambp) != 0)
         {
            free(amb.c);
            return -1;
         }
         trunc |= ot;
         for (size_t i = 0; i < on; i++)
            if (!agg_push(&agg, &oe[i]))
            {
               free(oe);
               free(agg.e);
               free(amb.c);
               return -1;
            }
         free(oe);
      }
      if (crd_compute_in(project, opts, &agg, &trunc) != 0)
      {
         free(agg.e);
         free(amb.c);
         return -1;
      }
      *out_edges = agg.e;
      *out_n = agg.n;
      if (truncated)
         *truncated = trunc;
   }

   if (rc != 0)
   {
      free(amb.c);
      return rc;
   }
   if (out_amb)
   {
      *out_amb = amb.c;
      if (out_amb_n)
         *out_amb_n = amb.n;
   }
   else
      free(amb.c);
   return 0;
}

/* Public entry: edges only (ambiguous candidates ignored). */
int canonical_index_cross_repo_deps(const char *project, const xrepo_deps_opts_t *opts,
                                    xrepo_dep_edge_t **out_edges, size_t *out_n, int *truncated)
{
   return canonical_index_cross_repo_deps_ex(project, opts, out_edges, out_n, truncated, NULL, NULL);
}
