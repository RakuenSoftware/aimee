/* wfe_live_foreach.c -- the live foreach.workflow child spawner.
 *
 * When a parent "build" run reaches the foreach.workflow node, the engine's executor
 * (which owns the fan-IN aggregation via the DB parent<->child linkage) calls this
 * provider to fan OUT: one child "slice" work item per packet in the split
 * packet-plan. Each child is linked to its parent (db1_work_item_set_parent), seeded
 * with its packet as a proposal, and left ACTIVE for the autonomy driver to pick up
 * and run to terminal — at which point the parent's foreach node aggregates the
 * children's states and advances (all merged) or parks (a slice failed / still
 * running). Registered from wfe_autonomy_register.
 */
#include "aimee.h"

#include "wfe_live_foreach.h"

#include "aimee_home.h"
#include "cJSON.h"
#include "db1.h"
#include "wfe_store.h"
#include "wfe_blocks.h"
#include "wfe_engine.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void serr(char *err, size_t errlen, const char *fmt, ...)
{
   if (!err || !errlen)
      return;
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(err, errlen, fmt, ap);
   va_end(ap);
}

/* Read a whole text file into a malloc'd NUL-terminated buffer (<= 1 MiB). NULL on
 * error / empty path. Caller frees. */
static char *read_file(const char *path)
{
   if (!path || !path[0])
      return NULL;
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (sz < 0 || sz > (1 << 20))
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   return buf;
}

/* Write `content` to `path` (truncating). Returns 0 on success. */
static int write_file(const char *path, const char *content)
{
   FILE *f = fopen(path, "wb");
   if (!f)
      return -1;
   size_t len = strlen(content);
   size_t wr = fwrite(content, 1, len, f);
   return (fclose(f) == 0 && wr == len) ? 0 : -1;
}

int wfe_foreach_spawn(const char *parent_wi, const char *child_workflow, const char *packets_path,
                      int max_children, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!parent_wi || !parent_wi[0])
      return -1;
   const char *cwf = (child_workflow && child_workflow[0]) ? child_workflow : "slice";

   /* Idempotent: if this parent already has children, don't re-spawn. */
   int existing = 0;
   if (db1_work_item_child_counts(parent_wi, &existing, NULL, NULL) == 0 && existing > 0)
      return existing;

   /* Read + parse the packet-plan. An absent plan means "no packets" (0 children), not
    * an error — the foreach executor advances past an empty fan-out. */
   char *doc = read_file(packets_path);
   if (!doc)
      return 0;
   cJSON *root = cJSON_Parse(doc);
   free(doc);
   if (!root)
   {
      serr(err, errlen, "packet-plan is not valid JSON");
      return -1;
   }
   const cJSON *packets = cJSON_GetObjectItemCaseSensitive(root, "packets");
   int npk = (packets && cJSON_IsArray(packets)) ? cJSON_GetArraySize(packets) : 0;
   if (npk <= 0)
   {
      cJSON_Delete(root);
      return 0;
   }
   if (max_children > 0 && npk > max_children)
   {
      /* An over-cap decomposition is itself a coordinator failure — refuse rather
       * than silently drop packets (no silent truncation). */
      serr(err, errlen, "packet-plan has %d packets (> cap %d)", npk, max_children);
      cJSON_Delete(root);
      return -1;
   }

   /* Parent repo (children inherit it). */
   db1_work_item_t parent;
   if (db1_work_item_get(parent_wi, &parent) != 1)
   {
      serr(err, errlen, "parent work item not found");
      cJSON_Delete(root);
      return -1;
   }

   /* Resolve the child workflow's version + start stage once (all children share it). */
   char cname[64], cver[65], cstart[64], crepo[512], cid_tmp[80], rerr[200] = "";
   if (wfe_work_item_resolve(cwf, parent.repo, cname, cver, cstart, crepo, cid_tmp, rerr,
                             sizeof rerr) != 0)
   {
      serr(err, errlen, "child workflow '%s': %s", cwf, rerr[0] ? rerr : "resolve failed");
      cJSON_Delete(root);
      return -1;
   }

   /* Directory for the per-child seed proposals (stable across the child's worktree). */
   char dir[1024];
   snprintf(dir, sizeof dir, "%s/wfe-packets", aimee_home());
   mkdir(dir, 0700);

   /* Create the children ALL-OR-NOTHING: a partial fan-out (some children created,
    * then an error) would let the foreach node aggregate the partial set as complete
    * and advance with fewer slices (and the idempotency guard would block a re-spawn).
    * So on any fatal error we delete every child we created and return -1, leaving the
    * parent child-less -> the executor re-spawns cleanly. */
   int created = 0;
   int failed = 0;
   for (int i = 0; i < npk && !failed; i++)
   {
      const cJSON *pk = cJSON_GetArrayItem(packets, i);
      if (!cJSON_IsObject(pk))
         continue;
      const cJSON *pid = cJSON_GetObjectItemCaseSensitive(pk, "packet_id");
      const cJSON *sum = cJSON_GetObjectItemCaseSensitive(pk, "summary");
      char child_id[80];
      snprintf(child_id, sizeof child_id, "%s.s%d", parent_wi, i);

      /* Seed the child with its packet as a proposal the slice's `understand` reads. */
      char proposal_path[1200];
      snprintf(proposal_path, sizeof proposal_path, "%s/%s.md", dir, child_id);
      char *pj = cJSON_PrintUnformatted(pk);
      char seed[4096];
      snprintf(seed, sizeof seed, "# Slice packet %s\n\n%s\n\n%s\n",
               (pid && cJSON_IsString(pid)) ? pid->valuestring : "",
               (sum && cJSON_IsString(sum)) ? sum->valuestring : "", pj ? pj : "{}");
      free(pj);
      if (write_file(proposal_path, seed) != 0)
      {
         serr(err, errlen, "could not seed child %s", child_id);
         failed = 1;
         break;
      }
      if (db1_work_item_create(child_id, crepo, proposal_path, cname, cver, cstart, "autonomous") !=
          0)
      {
         serr(err, errlen, "could not create child %s", child_id);
         failed = 1;
         break;
      }
      /* Link the child to its parent so the foreach node can aggregate it. */
      (void)db1_work_item_set_parent(child_id, parent_wi);
      created++;
   }
   cJSON_Delete(root);
   if (failed)
   {
      /* roll back every child created this call (deterministic ids par.s0..par.s{k-1}). */
      for (int i = 0; i < created; i++)
      {
         char cid[80];
         snprintf(cid, sizeof cid, "%s.s%d", parent_wi, i);
         (void)db1_work_item_delete(cid);
      }
      return -1;
   }
   aimee_log(LOG_INFO, "wfe-foreach", "spawned %d slice children for %s", created, parent_wi);
   return created;
}

/* ---- provider registration ---- */
static int live_spawn(const char *work_item_id, const char *child_workflow,
                      const char *packets_path, int max_children, char *err, size_t errlen)
{
   return wfe_foreach_spawn(work_item_id, child_workflow, packets_path, max_children, err, errlen);
}

static const wfe_foreach_provider_t WFE_LIVE_FOREACH = {live_spawn};

void wfe_live_foreach_register(void)
{
   wfe_set_foreach_provider(&WFE_LIVE_FOREACH);
   aimee_log(LOG_INFO, "wfe-foreach", "live slice-spawn provider registered");
}
