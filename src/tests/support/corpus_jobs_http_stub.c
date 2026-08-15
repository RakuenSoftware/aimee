/* corpus_jobs_http_stub.c: corpus pipeline stubs for kb_http route tests. */

#include "corpus_jobs.h"
#include "modules/db2/c/artifacts.h"

#include <stdio.h>
#include <string.h>

int db2_corpus_pipeline_status(db2_corpus_pipeline_stats_t *out)
{
   if (out)
      *out = (db2_corpus_pipeline_stats_t){.pending = 2, .running = 1, .complete = 3, .total = 6};
   return 0;
}

int db2_corpus_pipeline_drain(int limit, db2_corpus_pipeline_stats_t *out)
{
   (void)limit;
   if (out)
      *out = (db2_corpus_pipeline_stats_t){.complete = 6, .total = 6, .processed = 3};
   return 0;
}

int db2_corpus_pipeline_stage_counts(db2_corpus_pipeline_stage_count_t *out, int max_out)
{
   if (!out || max_out < 2)
      return -1;
   snprintf(out[0].stage, sizeof(out[0].stage), "ingested");
   snprintf(out[0].stage_status, sizeof(out[0].stage_status), "pending");
   out[0].count = 2;
   snprintf(out[1].stage, sizeof(out[1].stage), "complete");
   snprintf(out[1].stage_status, sizeof(out[1].stage_status), "complete");
   out[1].count = 3;
   return 2;
}

/* Canned facet hit so the /v1/search filter route is exercisable without a live
 * DB2 (precision of the real db2_artifact_filter_facets is covered by
 * test_artifacts.c). Echoes the requested kind so the wiring is observable. */
int db2_artifact_filter_facets(int64_t release_id, const char *project, const char *kind,
                               const char *status, const char *priority, const char *component,
                               db2_artifact_row_t *out, int max)
{
   (void)release_id;
   (void)project;
   (void)status;
   (void)priority;
   (void)component;
   if (!out || max < 1)
      return 0;
   memset(&out[0], 0, sizeof(out[0]));
   snprintf(out[0].id, sizeof(out[0].id), "art-facet-1");
   snprintf(out[0].kind, sizeof(out[0].kind), "%s", (kind && kind[0]) ? kind : "doc_summary");
   snprintf(out[0].payload_json, sizeof(out[0].payload_json), "{\"summary\":\"stub facet hit\"}");
   return 1;
}

int db2_artifact_filter_facets_scoped(int64_t release_id, const char *project,
                                      const char *exclude_project, const char *kind,
                                      const char *status, const char *priority,
                                      const char *component, db2_artifact_row_t *out, int max)
{
   int n = db2_artifact_filter_facets(release_id, project, kind, status, priority, component, out,
                                      max);
   if (n > 0)
   {
      snprintf(out[0].scope_kind, sizeof(out[0].scope_kind), "project");
      snprintf(out[0].scope_id, sizeof(out[0].scope_id), "%s",
               project && project[0] ? project : (exclude_project ? "proj-other" : ""));
      if (exclude_project && exclude_project[0])
         snprintf(out[0].id, sizeof(out[0].id), "art-facet-other");
   }
   return n;
}

/* Stub active release so the /v1/search release-binding wiring is observable. */
int64_t db2_kb_release_get_active(void)
{
   return 7;
}
