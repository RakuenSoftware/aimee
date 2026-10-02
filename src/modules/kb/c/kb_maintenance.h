#pragma once
#include <stddef.h>

typedef struct
{
   double lambda;           /* per-kind decay rate per day, default 0.005 */
   double confidence_floor; /* minimum confidence after decay, default 0.10 */
   int min_age_days;        /* skip rows touched within this many days, default 7 */
   int orphan_prune_days;   /* orphan threshold in days, default 90 */
   int dry_run;             /* 1 = compute but do not commit */
} kb_maintenance_config_t;

typedef struct
{
   int rows_decayed;
   int orphans_pruned;
   long elapsed_ms;
   char run_id[64]; /* UUID-style id for kb_maintenance_runs table */
   char error[256];
} kb_maintenance_result_t;

int kb_maintenance_run(const kb_maintenance_config_t *cfg, kb_maintenance_result_t *out);
void kb_maintenance_config_defaults(kb_maintenance_config_t *cfg);
