/* memory_effective.c: canonical filter contract helpers and effective-importance
 * computation for the memory-public-contract proposal.
 *
 * See docs/proposals/accepted/memory-public-contract.md. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "aimee.h"
#include "cJSON.h"
#include "memory.h"

/* ------------------------------------------------------------------ */
/* Kind-specific half-life in days used by the decay term.            */
/* ------------------------------------------------------------------ */

static double kind_halflife_days(const char *kind)
{
   if (!kind || !kind[0])
      return 90.0;
   if (strcmp(kind, "scratch") == 0 || strcmp(kind, "working_memory") == 0)
      return 1.0;
   if (strcmp(kind, "episode") == 0 || strcmp(kind, "task") == 0)
      return 30.0;
   if (strcmp(kind, "preference") == 0 || strcmp(kind, "rule") == 0 ||
       strcmp(kind, "policy") == 0 || strcmp(kind, "procedure") == 0)
      return 180.0;
   /* fact, decision, and everything else */
   return 90.0;
}

/* ------------------------------------------------------------------ */
/* memory_effective_importance                                         */
/* ------------------------------------------------------------------ */

double memory_effective_importance(const memory_t *m, time_t now_sec)
{
   if (!m)
      return 0.0;

   if (now_sec == 0)
      now_sec = time(NULL);

   /* Base signal: product of confidence and a salience floor. */
   double conf = m->confidence > 0.0 ? m->confidence : 0.5;
   if (conf > 1.0)
      conf = 1.0;
   double sal = m->salience > 0.0 ? m->salience : 0.5;
   if (sal > 1.0)
      sal = 1.0;
   double base = conf * sal;

   /* Age in days from created_at. */
   double age_days = 0.0;
   double age_sec = 0.0;
   if (m->created_at[0])
   {
      struct tm tm_val;
      memset(&tm_val, 0, sizeof(tm_val));
      if (strptime(m->created_at, "%Y-%m-%dT%H:%M:%SZ", &tm_val) ||
          strptime(m->created_at, "%Y-%m-%d %H:%M:%S", &tm_val))
      {
         tm_val.tm_isdst = -1;
         time_t created = mktime(&tm_val);
         if (created > 0 && now_sec >= created)
         {
            age_sec = (double)(now_sec - created);
            age_days = age_sec / 86400.0;
         }
      }
   }

   /* Kind-specific exponential decay: exp(-age / halflife). */
   double halflife = kind_halflife_days(m->kind);
   double decay = exp(-age_days / halflife);

   /* Freshness floor: rows < 24 h old never decay below (1 - base)
    * to avoid demoting brand-new rows below older reinforced rows. */
   if (age_sec < 86400.0)
   {
      double floor_val = 1.0 - base;
      if (decay < floor_val)
         decay = floor_val;
   }

   /* Bounded reinforcement: max multiplier 2.0, saturates slowly. */
   double use = m->use_count > 0 ? (double)m->use_count : 0.0;
   double reinforcement = 1.0 + 0.3 * log1p(use);
   if (reinforcement > 2.0)
      reinforcement = 2.0;

   double result = base * decay * reinforcement;
   if (result > 1.0)
      result = 1.0;
   if (result < 0.0)
      result = 0.0;
   return result;
}

/* ------------------------------------------------------------------ */
/* Canonical filter construction                                       */
/* ------------------------------------------------------------------ */

void memory_filter_from_scope(const char *scope_type, const char *scope_value, memory_filter_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));

   /* Scope slot: map a single (type, value) pair onto the spec. */
   if (scope_type && scope_value && scope_value[0])
   {
      if (strcmp(scope_type, "workspace") == 0)
         snprintf(out->scope.workspace, sizeof(out->scope.workspace), "%s", scope_value);
      else if (strcmp(scope_type, "project") == 0)
         snprintf(out->scope.project, sizeof(out->scope.project), "%s", scope_value);
      else if (strcmp(scope_type, "session") == 0)
         snprintf(out->scope.session, sizeof(out->scope.session), "%s", scope_value);
      else if (strcmp(scope_type, "user") == 0)
         snprintf(out->scope.user, sizeof(out->scope.user), "%s", scope_value);
      else
         /* Unknown type: store as workspace (existing behaviour). */
         snprintf(out->scope.workspace, sizeof(out->scope.workspace), "%s", scope_value);
   }

   out->visibility = MEMORY_VISIBILITY_DEFAULT;
}

/* ------------------------------------------------------------------ */
/* JSON serialization                                                  */
/* ------------------------------------------------------------------ */

struct cJSON *memory_filter_to_json(const memory_filter_t *f)
{
   if (!f)
      return cJSON_CreateNull();

   cJSON *obj = cJSON_CreateObject();

   /* Scope sub-object. */
   cJSON *scope = cJSON_CreateObject();
   cJSON_AddStringToObject(scope, "session", f->scope.session[0] ? f->scope.session : "ambient");
   cJSON_AddStringToObject(scope, "project", f->scope.project[0] ? f->scope.project : "ambient");
   cJSON_AddStringToObject(scope, "workspace",
                           f->scope.workspace[0] ? f->scope.workspace : "ambient");
   cJSON_AddStringToObject(scope, "user", f->scope.user[0] ? f->scope.user : "ambient");
   cJSON_AddItemToObject(obj, "scope", scope);

   /* Visibility string. */
   const char *vis_str = "default";
   if (f->visibility == MEMORY_VISIBILITY_STRICT)
      vis_str = "strict";
   else if (f->visibility == MEMORY_VISIBILITY_EXPANDED)
      vis_str = "expanded";
   cJSON_AddStringToObject(obj, "visibility", vis_str);

   /* Tier filter array. */
   if (f->tier_count > 0)
   {
      cJSON *tiers = cJSON_CreateArray();
      for (int i = 0; i < f->tier_count; i++)
         cJSON_AddItemToArray(tiers, cJSON_CreateString(f->tiers[i]));
      cJSON_AddItemToObject(obj, "tiers", tiers);
   }

   /* Kind filter array. */
   if (f->kind_count > 0)
   {
      cJSON *kinds = cJSON_CreateArray();
      for (int i = 0; i < f->kind_count; i++)
         cJSON_AddItemToArray(kinds, cJSON_CreateString(f->kinds[i]));
      cJSON_AddItemToObject(obj, "kinds", kinds);
   }

   /* Temporal as-of. */
   if (f->as_of[0])
      cJSON_AddStringToObject(obj, "as_of", f->as_of);

   return obj;
}
