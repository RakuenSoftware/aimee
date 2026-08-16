/* kb_calibrate.c: Bayesian promotion-threshold calibration runner.
 * See docs/proposals/done/bayesian-promotion-threshold-calibration.md */

#include "kb_calibrate.h"
#include "modules/db2/c/calibration.h"
#include "modules/db2/c/artifacts.h"
#include "headers/log.h"
#include "platform_process.h"
#include <cJSON.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAL_MIN_ROWS     20 /* minimum audit rows before fitting */
#define CAL_MAX_SURFACES 64

/* Enumerate surfaces that have enough audit data to calibrate. */
static int enumerate_surfaces(db2_calibration_surface_t *out, int max)
{
   return db2_calibration_surface_list(CAL_MIN_ROWS, out, max);
}

static int build_sidecar_request(const db2_calibration_surface_t *surface,
                                 const db2_calibration_bucket_t *buckets, int n_buckets,
                                 const db2_calibration_conformal_row_t *conformal_rows,
                                 int n_conformal, char *buf, size_t len)
{
   cJSON *req = cJSON_CreateObject();
   /* Copied out: both are added to the JSON request and read again below, across
    * other accessor calls. */
   char mv[CONFIG_COPY_MAX];
   char pv[CONFIG_COPY_MAX];
   config_calibration_model_version_copy(mv, sizeof(mv));
   config_calibration_prompt_version_copy(pv, sizeof(pv));
   const char *model_version = mv[0] ? mv : "beta-binomial-v1";
   const char *prompt_version = pv[0] ? pv : "v1";
   char feature_set_version[160];
   snprintf(feature_set_version, sizeof(feature_set_version), "%s/%s", prompt_version,
            model_version);
   cJSON_AddNumberToObject(req, "version", 1);
   cJSON_AddStringToObject(req, "role", "calibrate");
   cJSON_AddStringToObject(req, "model_version", model_version);
   cJSON_AddStringToObject(req, "prompt_version", prompt_version);

   cJSON *scope = cJSON_CreateObject();
   cJSON_AddStringToObject(scope, "kind", surface->scope_kind[0] ? surface->scope_kind : "global");
   cJSON_AddStringToObject(scope, "id", surface->scope_id);
   cJSON_AddItemToObject(req, "scope", scope);

   cJSON *inputs = cJSON_CreateObject();
   cJSON_AddStringToObject(inputs, "target_surface", surface->target_surface);
   cJSON_AddStringToObject(inputs, "kind", surface->kind);
   cJSON_AddStringToObject(inputs, "feature_set_version", feature_set_version);
   cJSON *feature_names = cJSON_CreateArray();
   cJSON_AddItemToArray(feature_names, cJSON_CreateString("sketch.frequency_kind_scope"));
   cJSON_AddItemToArray(feature_names, cJSON_CreateString("sketch.distinct_sources_hll"));
   cJSON_AddItemToObject(inputs, "sketch_feature_names", feature_names);

   cJSON *bkts = cJSON_CreateArray();
   for (int i = 0; i < n_buckets; i++)
   {
      cJSON *b = cJSON_CreateObject();
      cJSON *range = cJSON_CreateArray();
      cJSON_AddItemToArray(range, cJSON_CreateNumber(buckets[i].range_lo));
      cJSON_AddItemToArray(range, cJSON_CreateNumber(buckets[i].range_hi));
      cJSON_AddItemToObject(b, "range", range);
      cJSON_AddNumberToObject(b, "n_accepted", (int)buckets[i].alpha);
      cJSON_AddNumberToObject(b, "n_rejected", (int)buckets[i].beta);
      cJSON_AddItemToArray(bkts, b);
   }
   cJSON_AddItemToObject(inputs, "buckets", bkts);

   cJSON *conformal = cJSON_CreateArray();
   for (int i = 0; i < n_conformal; i++)
   {
      cJSON *row = cJSON_CreateObject();
      cJSON_AddNumberToObject(row, "applied_confidence", conformal_rows[i].applied_confidence);
      cJSON_AddStringToObject(row, "verdict", conformal_rows[i].verdict);
      cJSON_AddItemToArray(conformal, row);
   }
   cJSON_AddItemToObject(inputs, "conformal_window", conformal);

   cJSON *config_j = cJSON_CreateObject();
   cJSON_AddNumberToObject(config_j, "prior_alpha0", config_calibration_prior_alpha0());
   cJSON_AddNumberToObject(config_j, "prior_beta0", config_calibration_prior_beta0());
   cJSON_AddNumberToObject(config_j, "buckets", n_buckets);
   cJSON_AddNumberToObject(config_j, "credible_delta", config_calibration_credible_delta());
   cJSON_AddNumberToObject(config_j, "conformal_window_size",
                           config_calibration_conformal_window());
   cJSON_AddNumberToObject(config_j, "conformal_epsilon", config_calibration_conformal_epsilon());
   if (strcmp(surface->target_surface, "memory") == 0)
   {
      cJSON_AddNumberToObject(config_j, "tau_auto", config_calibration_tau_memory_auto());
      cJSON_AddNumberToObject(config_j, "tau_flag", config_calibration_tau_memory_flag());
   }
   else if (strcmp(surface->target_surface, "working_profile") == 0)
   {
      cJSON_AddNumberToObject(config_j, "tau_auto", config_calibration_tau_working_profile_auto());
      cJSON_AddNumberToObject(config_j, "tau_flag", config_calibration_tau_working_profile_flag());
   }
   cJSON_AddItemToObject(inputs, "config", config_j);

   cJSON_AddItemToObject(req, "inputs", inputs);

   char *s = cJSON_PrintUnformatted(req);
   cJSON_Delete(req);
   if (!s)
      return -1;

   snprintf(buf, len, "%s", s);
   free(s);
   return 0;
}

int kb_calibrate_run(void)
{
   if (config_calibration_enabled() == 0)
      return 0;

   /* Enumerate surfaces to calibrate */
   db2_calibration_surface_t surfaces[CAL_MAX_SURFACES];
   int n_surfaces = enumerate_surfaces(surfaces, CAL_MAX_SURFACES);
   if (n_surfaces <= 0)
      return 0;

   int written = 0;
   int bucket_count = config_calibration_buckets();
   if (bucket_count < 2)
      bucket_count = DB2_CALIBRATION_BUCKETS;
   if (bucket_count > DB2_CALIBRATION_BUCKETS)
      bucket_count = DB2_CALIBRATION_BUCKETS;

   for (int i = 0; i < n_surfaces; i++)
   {
      const db2_calibration_surface_t *surf = &surfaces[i];

      /* Gather audit stats */
      db2_calibration_bucket_t buckets[DB2_CALIBRATION_BUCKETS];
      int n_filled = db2_calibration_audit_stats(
          surf->target_surface, surf->kind, surf->scope_kind[0] ? surf->scope_kind : NULL,
          surf->scope_id[0] ? surf->scope_id : NULL, config_calibration_conformal_window(), buckets,
          bucket_count);
      if (n_filled < 0)
         continue;

      /* Skip if insufficient data */
      int total_rows = 0;
      for (int j = 0; j < bucket_count; j++)
         total_rows += buckets[j].sample_n;

      if (total_rows < CAL_MIN_ROWS)
         continue;

      db2_calibration_conformal_row_t conformal_rows[DB2_CALIBRATION_CONFORMAL_MAX];
      int n_conformal = db2_calibration_conformal_window(
          surf->target_surface, surf->kind, surf->scope_kind[0] ? surf->scope_kind : NULL,
          surf->scope_id[0] ? surf->scope_id : NULL, config_calibration_conformal_window(),
          conformal_rows, DB2_CALIBRATION_CONFORMAL_MAX);
      if (n_conformal < 0)
         n_conformal = 0;

      char payload_buf[16384] = "{}";

      char calibration_command[CONFIG_COPY_MAX];
      config_calibration_command_copy(calibration_command, sizeof(calibration_command));
      if (calibration_command[0])
      {
         /* Call sidecar */
         char req_buf[32768];
         if (build_sidecar_request(surf, buckets, bucket_count, conformal_rows, n_conformal,
                                   req_buf, sizeof(req_buf)) != 0)
         {
            aimee_log(LOG_DEBUG, "calibration", "failed to build sidecar request for %s/%s",
                      surf->target_surface, surf->kind);
            continue;
         }

         char *out = NULL;
         size_t out_len = 0;
         int rc = platform_exec_pipe(calibration_command, req_buf, strlen(req_buf), &out, &out_len);
         if (rc != 0 || !out)
         {
            aimee_log(LOG_DEBUG, "calibration", "sidecar failed for %s/%s (exit %d)",
                      surf->target_surface, surf->kind, rc);
            free(out);
            continue;
         }

         /* Parse sidecar response */
         cJSON *resp = cJSON_ParseWithLength(out, out_len);
         free(out);
         if (!resp)
         {
            aimee_log(LOG_DEBUG, "calibration", "sidecar returned invalid JSON for %s/%s",
                      surf->target_surface, surf->kind);
            continue;
         }

         cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
         cJSON *profile = cJSON_GetObjectItemCaseSensitive(resp, "profile");
         if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0 ||
             !cJSON_IsObject(profile))
         {
            cJSON *err = cJSON_GetObjectItemCaseSensitive(resp, "error");
            aimee_log(LOG_DEBUG, "calibration", "sidecar error for %s/%s: %s", surf->target_surface,
                      surf->kind, (cJSON_IsString(err) ? err->valuestring : "unknown"));
            cJSON_Delete(resp);
            continue;
         }

         char *profile_str = cJSON_PrintUnformatted(profile);
         cJSON_Delete(resp);
         if (!profile_str)
            continue;

         snprintf(payload_buf, sizeof(payload_buf), "%s", profile_str);
         free(profile_str);
      }

      /* Write calibration_profile artifact */
      char art_id[64];
      /* Copied out for the same reason as in the request builder above. */
      char pv2[CONFIG_COPY_MAX];
      char mv2[CONFIG_COPY_MAX];
      config_calibration_prompt_version_copy(pv2, sizeof(pv2));
      config_calibration_model_version_copy(mv2, sizeof(mv2));
      const char *prompt_version = pv2[0] ? pv2 : "v1";
      const char *model_version = mv2[0] ? mv2 : "beta-binomial-v1";
      char feature_set_version[160];
      snprintf(feature_set_version, sizeof(feature_set_version), "%s/%s", prompt_version,
               model_version);
      int wrc = db2_calibration_profile_write(
          surf->target_surface, surf->kind, surf->scope_kind[0] ? surf->scope_kind : "global",
          surf->scope_id, feature_set_version, payload_buf, art_id, sizeof(art_id));
      if (wrc == 0)
      {
         written++;
         aimee_log(LOG_DEBUG, "calibration", "wrote calibration_profile for %s/%s id=%s",
                   surf->target_surface, surf->kind, art_id);
      }
      else
      {
         aimee_log(LOG_DEBUG, "calibration", "failed to write calibration_profile for %s/%s",
                   surf->target_surface, surf->kind);
      }
   }

   return written;
}

int kb_calibrate_consume_drift_signals(void)
{
   db2_artifact_proposed_t rows[32];
   int n = db2_artifact_list_proposed(NULL, 64, rows, 32);
   if (n <= 0)
      return 0;

   int consumed = 0;
   for (int i = 0; i < n; i++)
   {
      if (strcmp(rows[i].kind, "drift_signal") != 0)
         continue;
      if (db2_artifact_stamp_reflected(rows[i].id) == 0)
      {
         aimee_log(LOG_INFO, "calibration", "drift_signal consumed: id=%s payload=%s", rows[i].id,
                   rows[i].payload_json[0] ? rows[i].payload_json : "{}");
         consumed++;
      }
   }
   return consumed;
}
