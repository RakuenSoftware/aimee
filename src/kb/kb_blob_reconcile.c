/* kb_blob_reconcile.c: orphan-blob reconciliation sweep. See kb_blob_reconcile.h. */
#include "kb_blob_reconcile.h"

#include "db2/kb_payload.h" /* db2_kb_blob_ref_referenced */
#include "kb_blob_store.h"
#include "log.h"

#include <string.h>
#include <time.h>

typedef struct
{
   kb_blob_recon_stats_t *stats;
   int error;
   long long now;
   int grace_secs;
} recon_ctx_t;

/* Per-blob visitor: an unreferenced blob OLDER than the grace window is reclaimable; unlink it
 * and tally bytes. The blob was already returned by readdir, so unlinking it now is safe. */
static int recon_visit(const char *sha, long long bytes, long long mtime, void *vctx)
{
   recon_ctx_t *ctx = (recon_ctx_t *)vctx;
   ctx->stats->blobs_scanned++;
   if (ctx->grace_secs > 0 && mtime > 0 && ctx->now > 0 && (ctx->now - mtime) < ctx->grace_secs)
      return 0; /* too fresh — may be mid-insert; let a later sweep reclaim it if truly orphaned */
   int ref = db2_kb_blob_ref_referenced(sha);
   if (ref < 0)
   {
      ctx->error = 1;
      return 1; /* stop the sweep on a DB error rather than risk a wrong unlink */
   }
   if (ref == 1)
      return 0; /* still referenced — keep */

   ctx->stats->orphan_bytes_total += bytes;
   if (kb_blob_store_unlink(sha) == 0)
   {
      ctx->stats->orphans_unlinked++;
      ctx->stats->orphan_bytes_reclaimed += bytes;
   }
   return 0;
}

int kb_blob_reconcile_run(int alarm_mb, int grace_secs, kb_blob_recon_stats_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));
   recon_ctx_t ctx = {out, 0, (long long)time(NULL), grace_secs};
   long long visited = kb_blob_store_foreach(recon_visit, &ctx);
   if (visited < 0 || ctx.error)
      return -1;

   if (alarm_mb > 0)
   {
      long long threshold = (long long)alarm_mb * 1024 * 1024;
      if (out->orphan_bytes_total > threshold)
      {
         out->alarm_fired = 1;
         /* Surface a lagging/failing sweep BEFORE it becomes silent storage exhaustion. */
         LOG_WARN("kb.blob.recon",
                  "orphan blob bytes %lld exceed alarm threshold %lld (unlinked=%lld this sweep)",
                  out->orphan_bytes_total, threshold, out->orphans_unlinked);
      }
   }
   LOG_INFO("kb.blob.recon", "sweep: scanned=%lld orphans_unlinked=%lld reclaimed_bytes=%lld",
            out->blobs_scanned, out->orphans_unlinked, out->orphan_bytes_reclaimed);
   return 0;
}
