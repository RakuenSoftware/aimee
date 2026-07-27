/* kb_curator_pipeline.h: the curator stage registry + per-pass runner.
 *
 * The curator drain used to hardcode an if-chain of stages gated on the previous
 * stage doing no work — which starved downstream stages (index_claims,
 * detect_contradictions) behind a large extract backlog. This replaces that with a
 * data-driven ordered registry: a drain pass flows each work-item through every
 * enabled stage. It is also the seam a GUI pipeline editor toggles/reorders, so
 * different applications compose their own pipeline from the same stages. */
#ifndef DEC_KB_CURATOR_PIPELINE_H
#define DEC_KB_CURATOR_PIPELINE_H 1

#include <stddef.h>

#include "config.h"             /* config_t */
#include "kb_curator_extract.h" /* kb_curator_extract_opts_t */

/* Resource lane. Stages in different lanes are meant to run on independent
 * cadences/workers because they bottleneck on different hardware: the LLM lane
 * is GPU-bound and slow (one unit/pass); the index lane is CPU-bound and fast
 * (drains its queue each pass) so embedding/indexing never waits on the GPU. */
typedef enum
{
   KB_CURATOR_LANE_LLM = 0,   /* GPU/LLM: extract, resolve, synthesize */
   KB_CURATOR_LANE_INDEX = 1, /* CPU: embed/index, contradiction SQL */
} kb_curator_lane_t;

/* One stage in the curator pipeline. */
typedef struct
{
   const char *name;  /* stable id (config/GUI key), e.g. "index_claims" */
   const char *label; /* human status label, e.g. "index claims" */
   /* Enabled predicate; NULL => always on. Reads this stage's config flag. */
   int (*enabled)(const config_t *cfg);
   /* Worker: process one unit. Returns 1=did work, 0=queue empty, <0=error. */
   int (*run)(const kb_curator_extract_opts_t *opts);
   /* Max units to run per pass; <=0 is treated as 1. Cheap stages (embed/SQL,
    * e.g. index_claims) use a larger budget so a freshly-extracted document's
    * claims drain in the same pass instead of one-per-pass behind the backlog. */
   int budget;
   kb_curator_lane_t lane; /* resource lane; see kb_curator_lane_t */
} kb_curator_stage_desc_t;

/* Run one pass over `stages`: each enabled stage advances up to its budget (or
 * until its queue empties), in order, stopping at the first stage that errors.
 * Returns 1 if any stage did work (caller loops), 0 if all idle (caller sleeps),
 * -1 if a stage errored (caller backs off). `set_status`, if non-NULL, is invoked
 * with the stage label before each unit. `lane_filter` < 0 runs every lane; otherwise only
 * stages whose lane matches (so a per-lane worker thread can drain just its lane). */
int kb_curator_pipeline_run_pass(const kb_curator_stage_desc_t *stages, size_t n, int lane_filter,
                                 const config_t *cfg, const kb_curator_extract_opts_t *opts,
                                 void (*set_status)(const char *label));

#endif /* DEC_KB_CURATOR_PIPELINE_H */
