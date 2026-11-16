/* test_mirror_sync_wire.c: every workspace.mirror-sync request a client builds
 * from mirror_sync_next_slice fits the server's per-method body limit, measured
 * on the bytes cJSON actually writes. 2026-11-02: the MCP reverse channel cut
 * 512 KiB raw chunks against a 256 KiB limit, every sync of a working tree with
 * more than ~256 KiB of changes drew 413, and every Git tool call refused. */
#include "mirror_sync_wire.h"
#include "cJSON.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The envelope rc_ship_chunk / cli_v1_mirror_sync_chunked send, with a long
 * workspace root so the reserve is exercised. */
static size_t request_size(const char *slice, size_t len)
{
   char root[2048];
   memset(root, 'r', sizeof(root) - 1);
   root[0] = '/';
   root[sizeof(root) - 1] = '\0';
   char *z = malloc(len + 1);
   assert(z);
   memcpy(z, slice, len);
   z[len] = '\0';
   cJSON *req = cJSON_CreateObject();
   cJSON_AddStringToObject(req, "method", "workspace.mirror-sync");
   cJSON *args = cJSON_CreateArray();
   cJSON_AddItemToArray(args, cJSON_CreateString(root));
   cJSON_AddItemToObject(req, "args", args);
   cJSON_AddStringToObject(req, "head", "9834072da0f1c2b3a4d5e6f708192a3b4c5d6e7f");
   cJSON_AddStringToObject(req, "branch", "proposal/backup-google-drive");
   cJSON_AddStringToObject(req, "upstream", "origin/proposal/backup-google-drive");
   cJSON_AddStringToObject(req, "diff", z);
   cJSON_AddStringToObject(req, "transfer", "0123456789abcdef0123456789abcdef");
   cJSON_AddNumberToObject(req, "seq", 123456);
   cJSON_AddBoolToObject(req, "final", 1);
   char *wire = cJSON_PrintUnformatted(req);
   assert(wire);
   size_t n = strlen(wire);
   free(wire);
   cJSON_Delete(req);
   free(z);
   return n;
}

static void check_patch(const char *name, const char *patch, size_t total)
{
   size_t at = 0;
   int slices = 0;
   size_t largest = 0;
   while (at < total)
   {
      size_t len = mirror_sync_next_slice(patch + at, total - at);
      assert(len > 0);                      /* always makes progress */
      assert(len <= MIRROR_SYNC_CHUNK_MAX); /* small enough to retry */
      size_t size = request_size(patch + at, len);
      if (size > MIRROR_SYNC_MAX_BODY)
      {
         fprintf(stderr, "%s: slice %d is a %zu-byte request, over the %u-byte limit\n", name,
                 slices, size, (unsigned)MIRROR_SYNC_MAX_BODY);
         abort();
      }
      if (size > largest)
         largest = size;
      at += len;
      slices++;
   }
   assert(at == total);
   assert(mirror_sync_slice_count(patch, total) == slices);
   printf("  PASS: %s: %zu bytes in %d request(s), largest %zu\n", name, total, slices, largest);
}

static char *fill(size_t n, char c)
{
   char *p = malloc(n + 1);
   assert(p);
   memset(p, c, n);
   p[n] = '\0';
   return p;
}

int main(void)
{
   /* The working tree that hit 413: well over 256 KiB of ordinary patch. */
   const char *line = "+  \"lockfileVersion\": 3, \"packages\": { \"node_modules/x\": {} },\n";
   size_t ll = strlen(line);
   size_t total = 900 * 1024;
   char *patch = malloc(total + 1);
   assert(patch);
   for (size_t i = 0; i < total; i++)
      patch[i] = line[i % ll];
   patch[total] = '\0';
   check_patch("package-lock-like patch", patch, total);
   assert(mirror_sync_slice_count(patch, total) >= 4); /* 512 KiB slices would be 2 */
   free(patch);

   /* Worst cases for escaping: every byte doubles, or becomes six. */
   char *nl = fill(700 * 1024, '\n');
   check_patch("all newlines", nl, 700 * 1024);
   free(nl);
   char *quotes = fill(700 * 1024, '"');
   check_patch("all quotes", quotes, 700 * 1024);
   free(quotes);
   char *ctl = fill(300 * 1024, '\x01');
   check_patch("all control bytes", ctl, 300 * 1024);
   free(ctl);

   /* Small and empty: one request, and an empty patch has no slices. */
   check_patch("one line", "diff --git a/x b/x\n", 19);
   assert(mirror_sync_next_slice("", 0) == 0);
   assert(mirror_sync_slice_count("", 0) == 0);

   /* The server enforces the same constant the clients size by. */
   assert(MIRROR_SYNC_CHUNK_MAX < MIRROR_SYNC_MAX_BODY);
   assert(MIRROR_SYNC_ENVELOPE_RESERVE < MIRROR_SYNC_MAX_BODY);
   printf("mirror_sync_wire: all tests passed\n");
   return 0;
}
