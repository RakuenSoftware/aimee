/* db1/db1_cron_jobs.h: cron job mirror and run history. */
#pragma once

#include "config.h"

int db1_cron_job_upsert(const cron_job_t *job);
int db1_cron_job_get(const char *job_id, cron_job_t *out);
int db1_cron_jobs_load(cron_job_t *out, int max, int enabled_only);
int db1_cron_job_set_enabled(const char *job_id, int enabled);
int db1_cron_jobs_set_enabled_all(int enabled);
int db1_cron_job_delete(const char *job_id);
int db1_cron_job_record_run(const char *job_id, const char *status, int silent, int delivered,
                            const char *output, const char *error, const char *output_hash);
char *db1_cron_jobs_list_json(void);
char *db1_cron_job_history_json(const char *job_id, int limit);
char *db1_cron_job_latest_output(const char *job_id);
char *db1_cron_job_last_output_hash(const char *job_id);
