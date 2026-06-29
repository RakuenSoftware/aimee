/* task_rail.c: portable plan state machine (fold §8, P5). See task_rail.h. */
#include "task_rail.h"

#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
   if (!s)
      return NULL;
   size_t n = strlen(s);
   char *c = malloc(n + 1);
   if (c)
      memcpy(c, s, n + 1);
   return c;
}

void task_rail_init(task_rail_t *r)
{
   if (!r)
      return;
   r->objective = NULL;
   r->locked = 0;
   r->steps = NULL;
   r->count = 0;
   r->cap = 0;
}

void task_rail_free(task_rail_t *r)
{
   if (!r)
      return;
   free(r->objective);
   for (size_t i = 0; i < r->count; i++)
   {
      free(r->steps[i].title);
      free(r->steps[i].evidence);
   }
   free(r->steps);
   task_rail_init(r);
}

static int rail_push(task_rail_t *r, const char *title, task_step_state_t st, const char *ev)
{
   /* Allocate into temporaries and publish the slot only after every required
    * allocation succeeds, so a partial OOM neither leaks nor leaves a NULL-title
    * slot behind. */
   char *t = dup_str(title ? title : "");
   if (!t)
      return -1;
   char *e = NULL;
   if (ev)
   {
      e = dup_str(ev);
      if (!e)
      {
         free(t);
         return -1;
      }
   }
   if (r->count == r->cap)
   {
      size_t ncap = r->cap ? r->cap * 2 : 8;
      task_step_t *ns = realloc(r->steps, ncap * sizeof(*ns));
      if (!ns)
      {
         free(t);
         free(e);
         return -1;
      }
      r->steps = ns;
      r->cap = ncap;
   }
   task_step_t *s = &r->steps[r->count];
   s->title = t;
   s->evidence = e;
   s->state = st;
   r->count++;
   return 0;
}

int task_rail_start(task_rail_t *r, const char *objective, const char *const *titles, size_t n)
{
   if (!r || (n > 0 && !titles))
      return -1;
   task_rail_free(r);
   task_rail_init(r);
   r->objective = dup_str(objective ? objective : "");
   if (!r->objective)
   {
      task_rail_free(r);
      return -1; /* -1 leaves the rail empty/initial */
   }
   for (size_t i = 0; i < n; i++)
      if (rail_push(r, titles[i], TASK_STEP_PENDING, NULL) != 0)
      {
         task_rail_free(r);
         return -1;
      }
   r->locked = 1;
   return 0;
}

int task_rail_reserve(task_rail_t *r, size_t idx)
{
   if (!r || idx >= r->count || r->steps[idx].state != TASK_STEP_PENDING)
      return -1;
   r->steps[idx].state = TASK_STEP_RESERVED;
   return 0;
}

int task_rail_ack(task_rail_t *r, size_t idx, const char *evidence)
{
   if (!r || idx >= r->count)
      return -1;
   /* Duplicate replacement evidence FIRST; on OOM leave the step unchanged. */
   if (evidence)
   {
      char *e = dup_str(evidence);
      if (!e)
         return -1;
      free(r->steps[idx].evidence);
      r->steps[idx].evidence = e;
   }
   r->steps[idx].state = TASK_STEP_DONE; /* PENDING or RESERVED -> DONE (see header) */
   return 0;
}

long task_rail_next(const task_rail_t *r)
{
   if (!r)
      return -1;
   for (size_t i = 0; i < r->count; i++)
      if (r->steps[i].state != TASK_STEP_DONE)
         return (long)i;
   return -1;
}

size_t task_rail_done_count(const task_rail_t *r)
{
   size_t d = 0;
   if (r)
      for (size_t i = 0; i < r->count; i++)
         if (r->steps[i].state == TASK_STEP_DONE)
            d++;
   return d;
}

/* All-or-nothing: any cJSON allocation failure deletes root and returns NULL, so a
 * successful return is always a complete serialization (round-trip idempotent). */
char *task_rail_serialize(const task_rail_t *r)
{
   if (!r)
      return NULL;
   cJSON *root = cJSON_CreateObject();
   if (!root)
      return NULL;
   if (!cJSON_AddStringToObject(root, "objective", r->objective ? r->objective : "") ||
       !cJSON_AddBoolToObject(root, "locked", r->locked))
   {
      cJSON_Delete(root);
      return NULL;
   }
   cJSON *steps = cJSON_AddArrayToObject(root, "steps");
   if (!steps)
   {
      cJSON_Delete(root);
      return NULL;
   }
   for (size_t i = 0; i < r->count; i++)
   {
      cJSON *s = cJSON_CreateObject();
      if (!s || !cJSON_AddStringToObject(s, "title", r->steps[i].title ? r->steps[i].title : "") ||
          !cJSON_AddNumberToObject(s, "state", (double)r->steps[i].state) ||
          (r->steps[i].evidence && !cJSON_AddStringToObject(s, "evidence", r->steps[i].evidence)))
      {
         cJSON_Delete(s);
         cJSON_Delete(root);
         return NULL;
      }
      cJSON_AddItemToArray(steps, s);
   }
   char *out = cJSON_PrintUnformatted(root);
   cJSON_Delete(root);
   return out;
}

/* Validate + build into a temporary rail; only swap into *r on complete success,
 * so malformed/partial JSON or OOM never destroys the caller's existing state. */
int task_rail_restore(task_rail_t *r, const char *json)
{
   if (!r || !json)
      return -1;
   cJSON *root = cJSON_Parse(json);
   if (!root || !cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return -1;
   }
   cJSON *steps = cJSON_GetObjectItem(root, "steps");
   if (steps && !cJSON_IsArray(steps))
   {
      cJSON_Delete(root);
      return -1;
   }

   task_rail_t tmp;
   task_rail_init(&tmp);
   const char *obj = cJSON_GetStringValue(cJSON_GetObjectItem(root, "objective"));
   tmp.objective = dup_str(obj ? obj : "");
   if (!tmp.objective)
   {
      cJSON_Delete(root);
      return -1;
   }
   tmp.locked = cJSON_IsTrue(cJSON_GetObjectItem(root, "locked")) ? 1 : 0;

   cJSON *s;
   cJSON_ArrayForEach(s, steps)
   {
      if (!cJSON_IsObject(s))
         continue;
      const char *title = cJSON_GetStringValue(cJSON_GetObjectItem(s, "title"));
      const char *ev = cJSON_GetStringValue(cJSON_GetObjectItem(s, "evidence"));
      cJSON *st = cJSON_GetObjectItem(s, "state");
      task_step_state_t state = TASK_STEP_PENDING; /* unknown/out-of-range -> pending */
      if (cJSON_IsNumber(st))
      {
         int v = (int)st->valuedouble;
         if (v == TASK_STEP_RESERVED || v == TASK_STEP_DONE)
            state = (task_step_state_t)v;
      }
      if (rail_push(&tmp, title ? title : "", state, ev) != 0)
      {
         task_rail_free(&tmp);
         cJSON_Delete(root);
         return -1; /* OOM -> caller's rail untouched */
      }
   }
   cJSON_Delete(root);
   task_rail_free(r);
   *r = tmp; /* swap in only on complete success */
   return 0;
}
