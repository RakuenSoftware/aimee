/* mirror_sync_wire.h: request sizing for workspace.mirror-sync, shared by the
 * server's per-method body limit and both clients that ship a working-tree
 * patch (the MCP reverse channel and `aimee workspace mirror-sync`).
 *
 * The clients once sized chunks on their own: the CLI at 128 KiB, the MCP
 * channel at 512 KiB of raw patch, both under a 256 KiB per-method limit they
 * did not share. The 512 KiB chunks drew 413 on every sync of a working tree
 * with more than ~256 KiB of changes, and every Git tool call then refused
 * to run. A raw-byte budget is also not enough on its own: JSON escaping
 * doubles a newline, quote or backslash and turns other control bytes into
 * six, so a 128 KiB slice of a newline-heavy patch can still exceed the limit.
 * The slice is therefore sized by its escaped length. */
#ifndef AIMEE_MIRROR_SYNC_WIRE_H
#define AIMEE_MIRROR_SYNC_WIRE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* The server's body limit for one workspace.mirror-sync request. */
#define MIRROR_SYNC_MAX_BODY (256u * 1024u)

/* Room kept for everything in the request but the patch: method, the
 * workspace root (a path), head, branch, upstream, transfer id, seq, final. */
#define MIRROR_SYNC_ENVELOPE_RESERVE (16u * 1024u)

/* Raw bytes per slice, before the escaped-size check: keeps requests small
 * enough to retry cheaply even for a patch with nothing to escape. */
#define MIRROR_SYNC_CHUNK_MAX (128u * 1024u)

   /* Bytes cJSON writes for `c` inside a JSON string. */
   static inline size_t mirror_sync_escaped_width(unsigned char c)
   {
      switch (c)
      {
      case '"':
      case '\\':
      case '\b':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
         return 2;
      default:
         return c < 0x20 ? 6 : 1;
      }
   }

   /* Length of the next slice of `p` (avail bytes left): the longest prefix,
    * at most MIRROR_SYNC_CHUNK_MAX bytes, whose escaped size fits the body
    * limit less the envelope reserve. Returns 0 only when avail is 0. */
   static inline size_t mirror_sync_next_slice(const char *p, size_t avail)
   {
      const size_t budget = MIRROR_SYNC_MAX_BODY - MIRROR_SYNC_ENVELOPE_RESERVE;
      size_t used = 0, n = 0;
      while (n < avail && n < MIRROR_SYNC_CHUNK_MAX)
      {
         size_t w = mirror_sync_escaped_width((unsigned char)p[n]);
         if (used + w > budget)
            break;
         used += w;
         n++;
      }
      return n;
   }

   /* How many slices mirror_sync_next_slice cuts `total` bytes into. */
   static inline int mirror_sync_slice_count(const char *p, size_t total)
   {
      int count = 0;
      for (size_t at = 0; at < total; count++)
         at += mirror_sync_next_slice(p + at, total - at);
      return count;
   }

#ifdef __cplusplus
}
#endif

#endif
