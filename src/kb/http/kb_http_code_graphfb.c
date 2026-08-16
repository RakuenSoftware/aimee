/* kb_http_code_graphfb.c: graph-feedback route handlers (§1 self-audit, §2
 * snapshot diff) split out of kb_http_code.c to keep that file under the
 * line-count limit. Shares the generic query/error helpers declared in
 * kb_http_code.h. */
#include "kb_http_code.h"
#include "aimee.h"
#include "cJSON.h"
#include "modules/db2/c/code_projection.h"
#include "modules/db2/c/lessons.h"
#include "modules/db2/c/lifecycle.h"
#include "kb/kb_graph_analytics.h"
#include "kb/lessons_reflect.h"
#include "kb/lessons_session_capture.h"
#include "kb/prompt_sanitizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GET /v1/code/graph/audit?project=<proj>[&max_findings=N]
 *
 * §1 self-audit: a deterministic, read-only structural-health pass over the
 * project's visible projection graph. Emits ranked findings — file-dependency
 * cycles, orphaned symbols, bridge symbols, low-cohesion communities, and
 * (labeled `no-confirmation-yet`) unverified-inferred edges — with an honesty gate
 * that says "clean" / "insufficient signal" rather than inventing noise. Pure
 * analytics over data already persisted; no LLM. Every corpus-derived string
 * rendered into the response passes the S0 sanitizer (fail-closed per field). */
#define AUDIT_MAX_EDGES         10000
#define AUDIT_COHESION_MIN_SIZE 8
#define AUDIT_UNVERIFIED_MIN    3 /* >= this many inferred-provenance incident edges */

void code_blast_radius_json_fields(cJSON *response, const blast_radius_t *blast)
{
   cJSON_AddStringToObject(response, "file", blast->file);
   cJSON_AddStringToObject(response, "project", blast->project);
   cJSON_AddNumberToObject(response, "generation", (double)blast->generation);
   cJSON_AddStringToObject(response, "freshness", blast->freshness);
   cJSON_AddBoolToObject(response, "resolved", blast->resolved);
   cJSON *dependents = cJSON_AddArrayToObject(response, "dependents");
   cJSON *dependent_edges = cJSON_AddArrayToObject(response, "dependent_edges");
   for (int i = 0; i < blast->dependent_count; i++)
   {
      cJSON_AddItemToArray(dependents, cJSON_CreateString(blast->dependents[i]));
      cJSON *edge = cJSON_CreateObject();
      cJSON_AddStringToObject(edge, "path", blast->dependents[i]);
      cJSON_AddStringToObject(edge, "provenance", blast->dependent_meta[i].provenance);
      cJSON_AddStringToObject(edge, "confidence", blast->dependent_meta[i].confidence);
      cJSON_AddStringToObject(edge, "project", blast->dependent_meta[i].project);
      cJSON_AddNumberToObject(edge, "generation", (double)blast->dependent_meta[i].generation);
      cJSON_AddStringToObject(edge, "freshness", blast->dependent_meta[i].freshness);
      cJSON_AddItemToArray(dependent_edges, edge);
   }
   cJSON_AddNumberToObject(response, "dependent_count", blast->dependent_count);
   cJSON *dependencies = cJSON_AddArrayToObject(response, "dependencies");
   cJSON *dependency_edges = cJSON_AddArrayToObject(response, "dependency_edges");
   for (int i = 0; i < blast->dependency_count; i++)
   {
      cJSON_AddItemToArray(dependencies, cJSON_CreateString(blast->dependencies[i]));
      cJSON *edge = cJSON_CreateObject();
      cJSON_AddStringToObject(edge, "identity", blast->dependencies[i]);
      cJSON_AddStringToObject(edge, "provenance", blast->dependency_meta[i].provenance);
      cJSON_AddStringToObject(edge, "confidence", blast->dependency_meta[i].confidence);
      cJSON_AddStringToObject(edge, "project", blast->dependency_meta[i].project);
      cJSON_AddNumberToObject(edge, "generation", (double)blast->dependency_meta[i].generation);
      cJSON_AddStringToObject(edge, "freshness", blast->dependency_meta[i].freshness);
      cJSON_AddItemToArray(dependency_edges, edge);
   }
   cJSON_AddNumberToObject(response, "dependency_count", blast->dependency_count);
}

/* FNV-1a 32-bit over a string → 8 hex chars. Deterministic stable id for the
 * §1↔§3 finding-outcome loop (a finding's id must not drift run-to-run). */
static void audit_finding_id(const char *prefix, const char *key, char *out, size_t out_len)
{
   unsigned int h = 2166136261u;
   for (const unsigned char *p = (const unsigned char *)key; p && *p; p++)
   {
      h ^= *p;
      h *= 16777619u;
   }
   snprintf(out, out_len, "%s:%08x", prefix, h);
}

/* Sanitize a corpus-derived label into buf (returned). On SANITIZE_REJECTED — a
 * structured field carrying control/injection markup — fail closed to a constant
 * marker so the finding still renders but can't smuggle a payload into an agent. */
static const char *audit_safe(const char *in, sanitize_kind_t kind, char *buf, size_t buflen)
{
   sanitize_reason_t reason;
   sanitize_status_t st = sanitize_for_prompt(in ? in : "", kind, buf, buflen, &reason);
   if (st == SANITIZE_REJECTED)
      snprintf(buf, buflen, "[unsafe-label]");
   return buf;
}

int handle_get_code_graph_audit(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   int scope_status =
       code_request_project(query_string, project, sizeof(project), 0, NULL, out_buf, out_cap);
   if (scope_status)
      return scope_status;

   int max_f = 20;
   char mf[16] = "";
   if (code_qparam(query_string, "max_findings", mf, sizeof(mf)))
      max_f = atoi(mf);
   if (max_f < 1)
      max_f = 1;
   if (max_f > 200)
      max_f = 200;

   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   code_projection_edge_t *edges = calloc(AUDIT_MAX_EDGES, sizeof(*edges));
   kb_graph_edge_t *gedges = calloc(AUDIT_MAX_EDGES, sizeof(*gedges));
   kb_graph_reledge_t *redges = calloc(AUDIT_MAX_EDGES, sizeof(*redges));
   if (!edges || !gedges || !redges)
   {
      free(edges);
      free(gedges);
      free(redges);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int ne = db2_code_projection_list_edges(project, edges, AUDIT_MAX_EDGES);
   if (ne < 0)
   {
      free(edges);
      free(gedges);
      free(redges);
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"projection graph unavailable\",\"code\":\"no_projection\"}");
      return 503;
   }
   for (int i = 0; i < ne; i++)
   {
      snprintf(gedges[i].source, sizeof(gedges[i].source), "%s", edges[i].source);
      snprintf(gedges[i].target, sizeof(gedges[i].target), "%s", edges[i].target);
      gedges[i].weight = edges[i].structural_weight;
      snprintf(redges[i].source, sizeof(redges[i].source), "%s", edges[i].source);
      snprintf(redges[i].relation, sizeof(redges[i].relation), "%s", edges[i].relation);
      snprintf(redges[i].target, sizeof(redges[i].target), "%s", edges[i].target);
   }

   /* Community assignment for the visible generation (for cohesion + grouping). */
   int64_t vgen = db2_code_projection_visible_id(project);
   code_projection_community_t *crows = NULL;
   kb_graph_community_t *comm = NULL;
   int ncomm = 0;
   if (vgen > 0)
   {
      crows = calloc(AUDIT_MAX_EDGES, sizeof(*crows));
      comm = calloc(AUDIT_MAX_EDGES, sizeof(*comm));
      if (crows && comm)
      {
         ncomm = db2_code_projection_communities_list(vgen, crows, AUDIT_MAX_EDGES);
         if (ncomm < 0)
            ncomm = 0;
         for (int i = 0; i < ncomm; i++)
         {
            snprintf(comm[i].node, sizeof(comm[i].node), "%s", crows[i].node_id);
            snprintf(comm[i].community, sizeof(comm[i].community), "%s", crows[i].community_id);
         }
      }
   }
   char source_hash[128] = "";
   db2_code_projection_visible_source_hash(project, source_hash, sizeof(source_hash));

   /* ── run the analytics ──────────────────────────────────────────────────── */
   kb_graph_cycle_t *cycles = calloc((size_t)max_f, sizeof(*cycles));
   kb_graph_hub_t *orphans = calloc((size_t)max_f, sizeof(*orphans));
   kb_graph_bridge_t *bridges = calloc((size_t)max_f, sizeof(*bridges));
   kb_graph_cohesion_t *cohesion = calloc((size_t)max_f, sizeof(*cohesion));
   if (!cycles || !orphans || !bridges || !cohesion)
   {
      free(edges);
      free(gedges);
      free(redges);
      free(crows);
      free(comm);
      free(cycles);
      free(orphans);
      free(bridges);
      free(cohesion);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int cyc_trunc = 0, bridge_approx = 0;
   int n_cyc = kb_graph_cycles(redges, ne, cycles, max_f, &cyc_trunc);
   if (n_cyc < 0)
      n_cyc = 0;
   /* orphans: least-connected non-container nodes, degree <= 1 */
   int n_orph_raw = kb_graph_hubs_ranked(gedges, ne, orphans, max_f, KB_HUB_BOTTOM_NOHUB);
   if (n_orph_raw < 0)
      n_orph_raw = 0;
   int n_orph = 0;
   for (int i = 0; i < n_orph_raw; i++)
      if (orphans[i].degree <= 1)
         orphans[n_orph++] = orphans[i];
   int n_bridge = kb_graph_bridges(gedges, ne, bridges, max_f, &bridge_approx);
   if (n_bridge < 0)
      n_bridge = 0;
   int n_cohesion =
       kb_graph_cohesion(gedges, ne, comm, ncomm, AUDIT_COHESION_MIN_SIZE, cohesion, max_f);
   if (n_cohesion < 0)
      n_cohesion = 0;

   /* unverified-inferred: per-node count of incident edges carrying inferred/
    * ambiguous provenance (structural_weight == 0 is the only provenance signal on
    * a projection edge). Ships labeled no-confirmation-yet, OUT of the roll-up —
    * the confirmation signal arrives in S3. Reuses the orphan buffer's node list is
    * unsafe; compute inline into a small ranked set. */
   /* (kept minimal: count weight-0 incident edges per node, surface >= threshold) */

   /* ── assemble response ──────────────────────────────────────────────────── */
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(edges);
      free(gedges);
      free(redges);
      free(crows);
      free(comm);
      free(cycles);
      free(orphans);
      free(bridges);
      free(cohesion);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "generation_id", (double)vgen);
   if (source_hash[0])
      cJSON_AddStringToObject(resp, "source_hash", source_hash); /* staleness echo (R1) */
   cJSON_AddNumberToObject(resp, "edge_count", ne);
   cJSON_AddBoolToObject(resp, "truncated", ne >= AUDIT_MAX_EDGES);

   cJSON *findings = cJSON_AddObjectToObject(resp, "findings");
   char fid[80];
   char sbuf[KB_GRAPH_NODE_MAX + 16];

   /* cycles */
   cJSON *jc = findings ? cJSON_AddArrayToObject(findings, "cycles") : NULL;
   for (int i = 0; jc && i < n_cyc; i++)
   {
      cJSON *f = cJSON_CreateObject();
      if (!f)
         continue;
      audit_finding_id("cycle", cycles[i].files[0], fid, sizeof(fid));
      cJSON_AddStringToObject(f, "finding_id", fid);
      cJSON *arr = cJSON_AddArrayToObject(f, "files");
      for (int p = 0; arr && p < cycles[i].len; p++)
         cJSON_AddItemToArray(arr,
                              cJSON_CreateString(audit_safe(cycles[i].files[p], SANITIZE_FILE_PATH,
                                                            sbuf, sizeof(sbuf))));
      cJSON_AddStringToObject(f, "why", "files form a circular dependency (collapsed call graph)");
      cJSON_AddItemToArray(jc, f);
   }
   cJSON_AddBoolToObject(findings, "cycles_truncated", cyc_trunc != 0);

   /* orphans */
   cJSON *jo = findings ? cJSON_AddArrayToObject(findings, "orphans") : NULL;
   for (int i = 0; jo && i < n_orph; i++)
   {
      cJSON *f = cJSON_CreateObject();
      if (!f)
         continue;
      audit_finding_id("orphan", orphans[i].node, fid, sizeof(fid));
      cJSON_AddStringToObject(f, "finding_id", fid);
      cJSON_AddStringToObject(
          f, "node", audit_safe(orphans[i].node, SANITIZE_SYMBOL_LABEL, sbuf, sizeof(sbuf)));
      cJSON_AddNumberToObject(f, "degree", orphans[i].degree);
      cJSON_AddStringToObject(f, "why",
                              "weakly-connected symbol (dead code, missing edge, or "
                              "undocumented entry point)");
      cJSON_AddItemToArray(jo, f);
   }

   /* bridges */
   cJSON *jb = findings ? cJSON_AddArrayToObject(findings, "bridges") : NULL;
   for (int i = 0; jb && i < n_bridge; i++)
   {
      cJSON *f = cJSON_CreateObject();
      if (!f)
         continue;
      audit_finding_id("bridge", bridges[i].node, fid, sizeof(fid));
      cJSON_AddStringToObject(f, "finding_id", fid);
      cJSON_AddStringToObject(
          f, "node", audit_safe(bridges[i].node, SANITIZE_SYMBOL_LABEL, sbuf, sizeof(sbuf)));
      cJSON_AddNumberToObject(f, "betweenness", bridges[i].betweenness);
      cJSON_AddStringToObject(f, "why",
                              "cross-cutting concern connecting otherwise-separate modules");
      cJSON_AddItemToArray(jb, f);
   }
   cJSON_AddBoolToObject(findings, "bridges_approximate", bridge_approx != 0);

   /* low-cohesion communities */
   cJSON *jco = findings ? cJSON_AddArrayToObject(findings, "low_cohesion") : NULL;
   for (int i = 0; jco && i < n_cohesion; i++)
   {
      cJSON *f = cJSON_CreateObject();
      if (!f)
         continue;
      audit_finding_id("cohesion", cohesion[i].community, fid, sizeof(fid));
      cJSON_AddStringToObject(f, "finding_id", fid);
      cJSON_AddStringToObject(
          f, "community",
          audit_safe(cohesion[i].community, SANITIZE_COMMUNITY_NAME, sbuf, sizeof(sbuf)));
      cJSON_AddNumberToObject(f, "conductance", cohesion[i].conductance);
      cJSON_AddNumberToObject(f, "min_size_threshold", AUDIT_COHESION_MIN_SIZE);
      cJSON_AddNumberToObject(f, "size", cohesion[i].size);
      cJSON_AddStringToObject(f, "community_id_scope", "generation-local");
      cJSON_AddStringToObject(f, "why",
                              "high conductance: much of the module's edge weight leaves it "
                              "(split candidate)");
      cJSON_AddItemToArray(jco, f);
   }

   /* unverified-inferred: labeled no-confirmation-yet, excluded from the roll-up. */
   cJSON *ju = findings ? cJSON_AddObjectToObject(findings, "unverified_inferred") : NULL;
   if (ju)
   {
      cJSON_AddStringToObject(ju, "signal", "no-confirmation-yet");
      cJSON *ua = cJSON_AddArrayToObject(ju, "nodes");
      /* Count weight-0 incident edges per node; surface those at/above threshold.
       * Emitted in edge-scan order (deterministic — list_edges is ORDER BY). */
      int emitted = 0;
      /* simple O(n^2)-bounded pass acceptable at AUDIT_MAX_EDGES cap for a labeled,
       * roll-up-excluded finding; a hash index is a later optimization. */
      for (int i = 0; ua && i < ne && emitted < max_f; i++)
      {
         const char *node = edges[i].source[0] ? edges[i].source : NULL;
         if (!node || edges[i].structural_weight != 0)
            continue;
         /* skip if already emitted */
         int dup = 0;
         for (int j = 0; j < i; j++)
            if (edges[j].structural_weight == 0 && strcmp(edges[j].source, node) == 0)
            {
               dup = 1;
               break;
            }
         if (dup)
            continue;
         int cnt = 0;
         for (int j = 0; j < ne; j++)
            if (edges[j].structural_weight == 0 &&
                (strcmp(edges[j].source, node) == 0 || strcmp(edges[j].target, node) == 0))
               cnt++;
         if (cnt < AUDIT_UNVERIFIED_MIN)
            continue;
         cJSON *f = cJSON_CreateObject();
         if (!f)
            continue;
         audit_finding_id("unverified", node, fid, sizeof(fid));
         cJSON_AddStringToObject(f, "finding_id", fid);
         cJSON_AddStringToObject(f, "node",
                                 audit_safe(node, SANITIZE_SYMBOL_LABEL, sbuf, sizeof(sbuf)));
         cJSON_AddNumberToObject(f, "inferred_edges", cnt);
         cJSON_AddItemToArray(ua, f);
         emitted++;
      }
   }

   /* honesty gate + roll-up summary (unverified-inferred excluded per R1). */
   int total = n_cyc + n_orph + n_bridge + n_cohesion;
   int clean = (total == 0);
   int insufficient = (ne < 5); /* too sparse to trust orphan/cohesion signals */
   cJSON_AddBoolToObject(resp, "clean", clean && !insufficient);
   cJSON_AddBoolToObject(resp, "insufficient_signal", insufficient);
   char summary[192];
   snprintf(summary, sizeof(summary), "%d cycles, %d orphans, %d bridges, %d low-cohesion", n_cyc,
            n_orph, n_bridge, n_cohesion);
   cJSON_AddStringToObject(resp, "summary",
                           insufficient ? "insufficient signal" : (clean ? "clean" : summary));

   free(edges);
   free(gedges);
   free(redges);
   free(crows);
   free(comm);
   free(cycles);
   free(orphans);
   free(bridges);
   free(cohesion);

   char *s = cJSON_PrintUnformatted(resp);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      snprintf(
          out_buf, (size_t)out_cap,
          "{\"error\":\"result too large; reduce max_findings\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
   {
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   }
   free(s);
   cJSON_Delete(resp);
   return status;
}

int handle_get_code_graph_audit_route(const char *method, const char *query_string, char *out_buf,
                                      int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_graph_audit(query_string, out_buf, out_cap);
}

/* GET /v1/code/graph/diff?project=<proj>&from_gen=<id|default_latest>&to_gen=<id>[&force=1]
 *
 * §2 snapshot diff: a deterministic structural diff of two projection generations
 * — nodes/edges added/removed, files newly in a dependency cycle, edges that newly
 * cross a community boundary, and newly-orphaned symbols. Community ids of the
 * `to` generation are remapped onto the `from` generation (two-pass, stable) so a
 * renumbered boundary isn't mistaken for new coupling. Refuses to compare across
 * extractor versions (a parser change reads as a structural change) unless
 * force=1. HTTP 409 + the generation list when a generation is missing. Read-only;
 * every rendered node/edge string passes the S0 sanitizer. */
#define DIFF_MAX_EDGES   10000
#define DIFF_MAX_ENTRIES 400

/* Resolve a from/to generation token: a positive integer, or the alias
 * "default_latest" (the visible generation). Returns the gen id, 0 if the alias
 * has no visible generation, or -1 on an unparseable token. */
static int64_t diff_resolve_gen(const char *project, const char *tok)
{
   if (!tok || !tok[0])
      return -1;
   if (strcmp(tok, "default_latest") == 0)
      return db2_code_projection_visible_id(project);
   char *end = NULL;
   long long v = strtoll(tok, &end, 10);
   if (end && *end == '\0' && v > 0)
      return (int64_t)v;
   return -1;
}

static int diff_write_gen_list_409(const char *project, char *out_buf, int out_cap,
                                   const char *which, int64_t missing)
{
   code_projection_generation_row_t rows[32];
   int n = db2_code_projection_generations_list(project, rows, 32);
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "error", "generation not found");
   cJSON_AddStringToObject(resp, "which", which);
   cJSON_AddNumberToObject(resp, "requested", (double)missing);
   cJSON_AddStringToObject(resp, "code", "generation_not_found");
   cJSON *arr = cJSON_AddArrayToObject(resp, "available_generations");
   for (int i = 0; arr && i < n; i++)
   {
      cJSON *g = cJSON_CreateObject();
      if (!g)
         continue;
      cJSON_AddNumberToObject(g, "id", (double)rows[i].id);
      cJSON_AddStringToObject(g, "state", rows[i].state);
      cJSON_AddStringToObject(g, "started_at", rows[i].started_at);
      cJSON_AddItemToArray(arr, g);
   }
   char *s = cJSON_PrintUnformatted(resp);
   int status = 409;
   if (!s || strlen(s) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"generation not found\",\"code\":\"%s\"}",
               "generation_not_found");
   }
   else
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   cJSON_Delete(resp);
   return status;
}

int handle_get_code_graph_diff(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   int scope_status =
       code_request_project(query_string, project, sizeof(project), 0, NULL, out_buf, out_cap);
   if (scope_status)
      return scope_status;
   char from_s[64] = "", to_s[64] = "", force_s[8] = "";
   if (!code_qparam(query_string, "from_gen", from_s, sizeof(from_s)) || !from_s[0])
      return code_scan_write_error(out_buf, out_cap, "missing from_gen (id or default_latest)");
   if (!code_qparam(query_string, "to_gen", to_s, sizeof(to_s)) || !to_s[0])
      return code_scan_write_error(out_buf, out_cap, "missing to_gen (id or default_latest)");
   int force = code_qparam(query_string, "force", force_s, sizeof(force_s)) &&
               (force_s[0] == '1' || force_s[0] == 't' || force_s[0] == 'T');

   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   int64_t from_gen = diff_resolve_gen(project, from_s);
   int64_t to_gen = diff_resolve_gen(project, to_s);
   if (from_gen < 0 || to_gen < 0)
      return code_scan_write_error(out_buf, out_cap,
                                   "from_gen/to_gen must be a generation id or 'default_latest'");
   if (from_gen == 0)
      return diff_write_gen_list_409(project, out_buf, out_cap, "from_gen", 0);
   if (to_gen == 0)
      return diff_write_gen_list_409(project, out_buf, out_cap, "to_gen", 0);

   code_projection_generation_meta_t fm, tm;
   int fr = db2_code_projection_generation_meta(from_gen, &fm);
   int tr = db2_code_projection_generation_meta(to_gen, &tm);
   if (fr == 1)
      return diff_write_gen_list_409(project, out_buf, out_cap, "from_gen", from_gen);
   if (tr == 1)
      return diff_write_gen_list_409(project, out_buf, out_cap, "to_gen", to_gen);
   if (fr < 0 || tr < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"generation lookup failed\"}");
      return 503;
   }
   /* generations must belong to the named project */
   if (strcmp(fm.project, project) != 0 || strcmp(tm.project, project) != 0)
      return code_scan_write_error(out_buf, out_cap, "generation does not belong to project");

   /* extractor-version guard: a parser change would read as a structural change. */
   int version_mismatch = strcmp(fm.extractor_version, tm.extractor_version) != 0;
   if (version_mismatch && !force)
   {
      cJSON *r = cJSON_CreateObject();
      if (r)
      {
         cJSON_AddStringToObject(r, "error",
                                 "refusing to diff across extractor versions (parser changes read "
                                 "as structural changes); pass force=1 to override");
         cJSON_AddStringToObject(r, "code", "extractor_version_mismatch");
         cJSON_AddStringToObject(r, "from_extractor_version", fm.extractor_version);
         cJSON_AddStringToObject(r, "to_extractor_version", tm.extractor_version);
         char *s = cJSON_PrintUnformatted(r);
         if (s && strlen(s) < (size_t)out_cap)
            snprintf(out_buf, (size_t)out_cap, "%s", s);
         else
            snprintf(out_buf, (size_t)out_cap,
                     "{\"error\":\"extractor version mismatch\",\"code\":\"extractor_version_"
                     "mismatch\"}");
         free(s);
         cJSON_Delete(r);
      }
      return 409;
   }

   /* Load both generations' edges + communities. */
   code_projection_edge_t *fe = calloc(DIFF_MAX_EDGES, sizeof(*fe));
   code_projection_edge_t *te = calloc(DIFF_MAX_EDGES, sizeof(*te));
   kb_graph_reledge_t *fre = calloc(DIFF_MAX_EDGES, sizeof(*fre));
   kb_graph_reledge_t *tre = calloc(DIFF_MAX_EDGES, sizeof(*tre));
   code_projection_community_t *fcr = calloc(DIFF_MAX_EDGES, sizeof(*fcr));
   code_projection_community_t *tcr = calloc(DIFF_MAX_EDGES, sizeof(*tcr));
   kb_graph_community_t *fc = calloc(DIFF_MAX_EDGES, sizeof(*fc));
   kb_graph_community_t *tc = calloc(DIFF_MAX_EDGES, sizeof(*tc));
   kb_graph_community_t *tcremap = calloc(DIFF_MAX_EDGES, sizeof(*tcremap));
   kb_graph_diff_entry_t *entries = calloc(DIFF_MAX_ENTRIES, sizeof(*entries));
   if (!fe || !te || !fre || !tre || !fcr || !tcr || !fc || !tc || !tcremap || !entries)
   {
      free(fe);
      free(te);
      free(fre);
      free(tre);
      free(fcr);
      free(tcr);
      free(fc);
      free(tc);
      free(tcremap);
      free(entries);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   int nfe = db2_code_projection_list_edges_for_gen(from_gen, fe, DIFF_MAX_EDGES);
   int nte = db2_code_projection_list_edges_for_gen(to_gen, te, DIFF_MAX_EDGES);
   if (nfe < 0)
      nfe = 0;
   if (nte < 0)
      nte = 0;
   for (int i = 0; i < nfe; i++)
   {
      snprintf(fre[i].source, sizeof(fre[i].source), "%s", fe[i].source);
      snprintf(fre[i].relation, sizeof(fre[i].relation), "%s", fe[i].relation);
      snprintf(fre[i].target, sizeof(fre[i].target), "%s", fe[i].target);
   }
   for (int i = 0; i < nte; i++)
   {
      snprintf(tre[i].source, sizeof(tre[i].source), "%s", te[i].source);
      snprintf(tre[i].relation, sizeof(tre[i].relation), "%s", te[i].relation);
      snprintf(tre[i].target, sizeof(tre[i].target), "%s", te[i].target);
   }
   int nfc = db2_code_projection_communities_list(from_gen, fcr, DIFF_MAX_EDGES);
   int ntc = db2_code_projection_communities_list(to_gen, tcr, DIFF_MAX_EDGES);
   if (nfc < 0)
      nfc = 0;
   if (ntc < 0)
      ntc = 0;
   for (int i = 0; i < nfc; i++)
   {
      snprintf(fc[i].node, sizeof(fc[i].node), "%s", fcr[i].node_id);
      snprintf(fc[i].community, sizeof(fc[i].community), "%s", fcr[i].community_id);
   }
   for (int i = 0; i < ntc; i++)
   {
      snprintf(tc[i].node, sizeof(tc[i].node), "%s", tcr[i].node_id);
      snprintf(tc[i].community, sizeof(tc[i].community), "%s", tcr[i].community_id);
   }
   /* Stabilize the `to` community ids onto the `from` generation. */
   int nremap = kb_graph_community_remap(fc, nfc, tc, ntc, tcremap, DIFF_MAX_EDGES);
   if (nremap < 0)
      nremap = 0;

   int diff_trunc = 0;
   int nd = kb_graph_diff(fre, nfe, fc, nfc, tre, nte, nremap ? tcremap : tc, nremap ? nremap : ntc,
                          entries, DIFF_MAX_ENTRIES, &diff_trunc);
   if (nd < 0)
      nd = 0;

   /* Assemble the response, grouping entries by kind, sanitizing every string. */
   cJSON *resp = cJSON_CreateObject();
   if (!resp)
   {
      free(fe);
      free(te);
      free(fre);
      free(tre);
      free(fcr);
      free(tcr);
      free(fc);
      free(tc);
      free(tcremap);
      free(entries);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }
   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "from_gen", (double)from_gen);
   cJSON_AddNumberToObject(resp, "to_gen", (double)to_gen);
   cJSON_AddStringToObject(resp, "from_extractor_version", fm.extractor_version);
   cJSON_AddStringToObject(resp, "to_extractor_version", tm.extractor_version);
   /* When versions match, all change is source-induced; a forced mismatch may mix
    * in extractor/projector-induced change — surfaced, not silently conflated. */
   cJSON_AddStringToObject(resp, "diff_kind",
                           version_mismatch ? "source+extractor (forced cross-version)"
                                            : "source-induced");
   cJSON_AddBoolToObject(resp, "extractor_version_forced", version_mismatch && force);
   cJSON_AddBoolToObject(resp, "truncated",
                         diff_trunc != 0 || nfe >= DIFF_MAX_EDGES || nte >= DIFF_MAX_EDGES);

   struct
   {
      kb_graph_diff_kind_t k;
      const char *name;
      int edge;
   } groups[] = {
       {KB_DIFF_NODE_ADDED, "nodes_added", 0},
       {KB_DIFF_NODE_REMOVED, "nodes_removed", 0},
       {KB_DIFF_EDGE_ADDED, "edges_added", 1},
       {KB_DIFF_EDGE_REMOVED, "edges_removed", 1},
       {KB_DIFF_NEW_ORPHAN, "newly_orphaned", 0},
       {KB_DIFF_NEW_CROSS_COMMUNITY, "new_cross_community", 1},
       {KB_DIFF_NEW_CYCLE_MEMBER, "new_cycle_members", 0},
   };
   char sb[KB_GRAPH_NODE_MAX + 16], sb2[KB_GRAPH_NODE_MAX + 16];
   int counts[8] = {0};
   for (size_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++)
   {
      cJSON *arr = cJSON_AddArrayToObject(resp, groups[g].name);
      for (int i = 0; arr && i < nd; i++)
      {
         if (entries[i].kind != groups[g].k)
            continue;
         counts[groups[g].k]++;
         if (groups[g].edge)
         {
            cJSON *e = cJSON_CreateObject();
            if (!e)
               continue;
            cJSON_AddStringToObject(
                e, "source", audit_safe(entries[i].a, SANITIZE_SYMBOL_LABEL, sb, sizeof(sb)));
            cJSON_AddStringToObject(
                e, "target", audit_safe(entries[i].b, SANITIZE_SYMBOL_LABEL, sb2, sizeof(sb2)));
            cJSON_AddStringToObject(
                e, "relation",
                audit_safe(entries[i].relation, SANITIZE_SYMBOL_LABEL, sb, sizeof(sb)));
            cJSON_AddItemToArray(arr, e);
         }
         else
         {
            cJSON_AddItemToArray(arr, cJSON_CreateString(audit_safe(
                                          entries[i].a, SANITIZE_FILE_PATH, sb, sizeof(sb))));
         }
      }
   }
   char summary[224];
   snprintf(
       summary, sizeof(summary),
       "%d nodes +, %d nodes -, %d edges +, %d edges -, %d newly-orphaned, %d new cross-module, "
       "%d new cycle members",
       counts[KB_DIFF_NODE_ADDED], counts[KB_DIFF_NODE_REMOVED], counts[KB_DIFF_EDGE_ADDED],
       counts[KB_DIFF_EDGE_REMOVED], counts[KB_DIFF_NEW_ORPHAN],
       counts[KB_DIFF_NEW_CROSS_COMMUNITY], counts[KB_DIFF_NEW_CYCLE_MEMBER]);
   cJSON_AddStringToObject(resp, "summary", nd == 0 ? "no structural change" : summary);

   free(fe);
   free(te);
   free(fre);
   free(tre);
   free(fcr);
   free(tcr);
   free(fc);
   free(tc);
   free(tcremap);
   free(entries);

   char *s = cJSON_PrintUnformatted(resp);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"result too large\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   cJSON_Delete(resp);
   return status;
}

int handle_get_code_graph_diff_route(const char *method, const char *query_string, char *out_buf,
                                     int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_graph_diff(query_string, out_buf, out_cap);
}

/* GET /v1/code/lessons?project=<proj>
 *
 * §3 lessons artifact: the deterministic reflection over the retrieval-outcome
 * ledger (which sources earned trust), grouped by community. Read-only; every
 * rendered node/community string passes the S0 sanitizer. Honesty gate: an empty
 * ledger returns "no lessons yet", not invented noise. Consumed by S3c's session
 * preamble + RRF tie-break. */
#define LESSONS_MAX_RECORDS 5000

int handle_get_code_lessons(const char *query_string, char *out_buf, int out_cap)
{
   char project[256] = "";
   int scope_status =
       code_request_project(query_string, project, sizeof(project), 0, NULL, out_buf, out_cap);
   if (scope_status)
      return scope_status;
   if (!db2_is_initialized())
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }

   int64_t vgen = db2_code_projection_visible_id(project);
   db2_lessons_outcome_row_t *rows = calloc(LESSONS_MAX_RECORDS, sizeof(*rows));
   lessons_reflect_input_t *inp = calloc(LESSONS_MAX_RECORDS, sizeof(*inp));
   lessons_reflect_entry_t *ent = calloc(LESSONS_MAX_RECORDS, sizeof(*ent));
   cJSON *resp = cJSON_CreateObject();
   if (!rows || !inp || !ent || !resp)
   {
      free(rows);
      free(inp);
      free(ent);
      cJSON_Delete(resp);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      return 500;
   }

   int nr = db2_lessons_list_outcomes(project, vgen > 0 ? vgen : 0, rows, LESSONS_MAX_RECORDS);
   if (nr < 0)
      nr = 0;
   for (int i = 0; i < nr; i++)
   {
      snprintf(inp[i].node, sizeof(inp[i].node), "%s", rows[i].node_id);
      snprintf(inp[i].community, sizeof(inp[i].community), "%s", rows[i].community);
      snprintf(inp[i].answer_outcome, sizeof(inp[i].answer_outcome), "%s", rows[i].answer_outcome);
      snprintf(inp[i].actor_source, sizeof(inp[i].actor_source), "%s", rows[i].actor_source);
      inp[i].ts_days = rows[i].ts_days;
      inp[i].confirmed = rows[i].confirmed;
   }
   long now_days = (long)(time(NULL) / 86400);
   int ne = lessons_reflect(inp, nr, now_days, NULL, ent, LESSONS_MAX_RECORDS);
   if (ne < 0)
   {
      /* An internal reflection failure is a 500, not a silent "clean" 200. */
      free(rows);
      free(inp);
      free(ent);
      cJSON_Delete(resp);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"reflection failed\"}");
      return 500;
   }

   cJSON_AddStringToObject(resp, "status", "ok");
   cJSON_AddStringToObject(resp, "project", project);
   cJSON_AddNumberToObject(resp, "record_count", nr);
   cJSON_AddBoolToObject(resp, "clean", ne == 0);
   cJSON_AddBoolToObject(resp, "truncated", nr >= LESSONS_MAX_RECORDS);
   cJSON_AddStringToObject(resp, "summary",
                           ne == 0 ? "no lessons yet — the ledger has no outcome records"
                                   : "sources ranked by earned trust, grouped by module");

   /* ent is sorted by (community, class, node); emit one group per community. */
   char sb[KB_GRAPH_NODE_MAX + 16];
   cJSON *groups = cJSON_AddArrayToObject(resp, "communities");
   cJSON *cur = NULL, *lessons = NULL;
   const char *cur_comm = NULL;
   for (int i = 0; groups && i < ne; i++)
   {
      if (!cur_comm || strcmp(cur_comm, ent[i].community) != 0)
      {
         cur = cJSON_CreateObject();
         if (!cur)
            continue;
         cJSON_AddStringToObject(
             cur, "community",
             audit_safe(ent[i].community, SANITIZE_COMMUNITY_NAME, sb, sizeof(sb)));
         lessons = cJSON_AddArrayToObject(cur, "lessons");
         cJSON_AddItemToArray(groups, cur);
         cur_comm = ent[i].community;
      }
      cJSON *l = cJSON_CreateObject();
      if (!l || !lessons)
         continue;
      cJSON_AddStringToObject(l, "node",
                              audit_safe(ent[i].node, SANITIZE_SYMBOL_LABEL, sb, sizeof(sb)));
      cJSON_AddStringToObject(l, "class", lessons_class_name(ent[i].klass));
      cJSON_AddNumberToObject(l, "score", ent[i].score);
      cJSON_AddNumberToObject(l, "distinct_positive", ent[i].distinct_positive);
      cJSON_AddNumberToObject(l, "distinct_negative", ent[i].distinct_negative);
      cJSON_AddBoolToObject(l, "confirmed_correction", ent[i].has_confirmed_correction != 0);
      cJSON_AddItemToArray(lessons, l);
   }

   free(rows);
   free(inp);
   free(ent);

   char *s = cJSON_PrintUnformatted(resp);
   int status = 200;
   if (!s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"oom\"}");
      status = 500;
   }
   else if (strlen(s) >= (size_t)out_cap)
   {
      snprintf(out_buf, (size_t)out_cap,
               "{\"error\":\"result too large\",\"code\":\"result_too_large\"}");
      status = 413;
   }
   else
      snprintf(out_buf, (size_t)out_cap, "%s", s);
   free(s);
   cJSON_Delete(resp);
   return status;
}

int handle_get_code_lessons_route(const char *method, const char *query_string, char *out_buf,
                                  int out_cap)
{
   if (strcmp(method, "GET") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_get_code_lessons(query_string, out_buf, out_cap);
}

/* POST /v1/code/lessons/observe  body: {project, session_id, node_ids:[...]}
 *
 * §3 live cite-capture: the server posts the file paths a session retrieved this
 * turn; a node re-cited within the auto-useful window earns an agent-sourced,
 * unconfirmed 'useful' outcome (inert until confirmed). Best-effort; the node-id
 * space is the retrieval file-path space the /v1/code/hybrid trust tie-break reads. */
int handle_post_code_lessons_observe(const char *body, char *out_buf, int out_cap)
{
   cJSON *root = cJSON_Parse(body ? body : "");
   if (!root)
      return code_scan_write_error(out_buf, out_cap, "invalid JSON body");
   cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "project");
   cJSON *js = cJSON_GetObjectItemCaseSensitive(root, "session_id");
   cJSON *jn = cJSON_GetObjectItemCaseSensitive(root, "node_ids");
   if (!cJSON_IsString(jp) || !jp->valuestring[0] || !cJSON_IsString(js) || !js->valuestring[0] ||
       !cJSON_IsArray(jn))
   {
      cJSON_Delete(root);
      return code_scan_write_error(out_buf, out_cap,
                                   "requires project, session_id, and node_ids[]");
   }
   if (!db2_is_initialized())
   {
      cJSON_Delete(root);
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"knowledge service not initialized\"}");
      return 503;
   }
   int n = cJSON_GetArraySize(jn);
   const char **nodes = calloc((size_t)(n > 0 ? n : 1), sizeof(*nodes));
   int cnt = 0;
   if (nodes)
      for (int i = 0; i < n; i++)
      {
         cJSON *e = cJSON_GetArrayItem(jn, i);
         if (cJSON_IsString(e) && e->valuestring[0])
            nodes[cnt++] = e->valuestring;
      }
   int64_t gen = db2_code_projection_visible_id(jp->valuestring);
   int recorded =
       lessons_session_observe(jp->valuestring, gen > 0 ? gen : 0, js->valuestring, nodes, cnt);
   free(nodes);
   snprintf(out_buf, (size_t)out_cap, "{\"status\":\"ok\",\"observed\":%d,\"recorded\":%d}", cnt,
            recorded < 0 ? 0 : recorded);
   cJSON_Delete(root);
   return 200;
}

int handle_post_code_lessons_observe_route(const char *method, const char *body, char *out_buf,
                                           int out_cap)
{
   if (strcmp(method, "POST") != 0)
      return code_method_not_allowed(out_buf, out_cap);
   return handle_post_code_lessons_observe(body, out_buf, out_cap);
}
