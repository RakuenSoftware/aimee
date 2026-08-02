/* workspace_scan_indexed.h: did a scan actually index anything?
 *
 * Header-only because both sides of the wire need the same answer and must not
 * drift: the server decides what `workspace add` reports per project, and the
 * CLI decides what `index scan` prints. They disagreed — `workspace add` warned
 * that kb had seen no files while `index scan` of the same tree printed a bare
 * "Scan complete", so the same broken state read as a failure through one
 * command and a success through the other.
 *
 * kb is handed a filesystem PATH. In the managed topology aimee-server and
 * aimee-kb are separate containers, and kb additionally reads as its own uid, so
 * a path the server can read may be empty or unreadable for kb. kb then walks
 * nothing, finds no files, and answers success. */
#ifndef DEC_WORKSPACE_SCAN_INDEXED_H
#define DEC_WORKSPACE_SCAN_INDEXED_H 1

static inline int workspace_scan_indexed(int rc, int skipped, int inspected, int files)
{
   if (rc != 0 || skipped)
      return 0;
   /* `inspected` is the right test, not `files`: a project already indexed and
    * unchanged legitimately reports files == 0 with inspected > 0, and calling
    * that "not indexed" would be its own wrong answer. inspected == 0 means kb
    * saw no files to consider. Older kb builds do not report inspected
    * (documented as 0), so fall back to files rather than calling a working
    * older kb broken. */
   int visited = inspected > 0 ? inspected : files;
   return visited > 0;
}

/* The one sentence a user can act on when a scan indexed nothing. Shared so the
 * server's per-project `reason` and the CLI's scan output say the same thing. */
#define WORKSPACE_SCAN_EMPTY_REASON                                                                \
   "knowledge service saw no files at that path — it may not be able to read it "                  \
   "(aimee-kb runs in its own container and does not share the server's filesystem)"

#endif /* DEC_WORKSPACE_SCAN_INDEXED_H */
