/* kb_blob_reconcile.h: orphan-blob reconciliation for the structured-PDF blob store
 * (structured-PDF Phase C, RT-S2/RT2-perf). Deletion is NOT a relational cascade: when a
 * document's asset rows are deleted/re-ingested, this periodic sweep reclaims blobs that NO
 * kb_doc_assets row references (refcount-by-scan), so a shared/deduped blob survives until its
 * last referrer is gone. The brief window between row deletion and blob unlink is a bounded
 * orphan window with no correctness or access impact. */
#ifndef AIMEE_KB_BLOB_RECONCILE_H
#define AIMEE_KB_BLOB_RECONCILE_H

typedef struct
{
   long long blobs_scanned;
   long long orphans_unlinked;
   long long orphan_bytes_reclaimed;
   long long orphan_bytes_total; /* reclaimable bytes seen this sweep (drives the alarm) */
   int alarm_fired;              /* 1 if orphan_bytes_total exceeded the configured threshold */
} kb_blob_recon_stats_t;

/* Run one reconciliation sweep. alarm_mb is the orphan-bytes alarm threshold in MiB (<=0
 * disables the alarm). Unlinks every blob with no kb_doc_assets referrer. Returns 0 on success
 * (stats populated), -1 on error. */
int kb_blob_reconcile_run(int alarm_mb, kb_blob_recon_stats_t *out);

#endif /* AIMEE_KB_BLOB_RECONCILE_H */
