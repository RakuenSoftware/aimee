/* db1/fsnap.h: file-snapshot checkpoints for session rewind.
 *
 * Pure domain API. No backend types or handles in any signature.
 * Implementation lives in src/db1/fsnap.c.
 *
 * A file snapshot captures the on-disk content of a set of files at a
 * point in time. Rewinding restores those files to the captured state
 * (writing missing files, overwriting changed ones, deleting files that
 * did not exist when the snapshot was taken). */
#ifndef DEC_DB1_FSNAP_H
#define DEC_DB1_FSNAP_H 1

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define FSNAP_MAX_PER_SESSION_DEFAULT 50

   typedef struct
   {
      int64_t id;
      int turn;
      char session_id[64];
      char created_at[32];
      char label[128];
      int file_count;
   } fsnap_info_t;

   /* Return an existing snapshot id for (session_id, turn, label), or
    * create one if none exists. Used for auto-snapshots so all file
    * writes in a single turn share one snapshot. */
   int64_t db1_fsnap_get_or_create(const char *session_id, int turn, const char *label);

   /* Record the current content of `path` as part of snapshot `snap_id`.
    * Safe to call multiple times per path (the latest call wins). */
   int db1_fsnap_record_file(int64_t snap_id, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* DEC_DB1_FSNAP_H */
