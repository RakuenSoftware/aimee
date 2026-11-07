/* Generic startup-hook memory/feature-off boundaries, not human authentication. */
#include "server/host_original_text.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static int resolves, transforms, mode;
static host_text_status resolve(host_text_kind kind, host_text_origin *out, void *state)
{
   (void)state;
   resolves++;
   out->schema_version = 1;
   out->kind = kind;
   strcpy(out->operation_uuid, "12345678-1234-4234-8234-123456789abc");
   strcpy(out->principal, "synthetic");
   strcpy(out->transport_identity, "fixture");
   strcpy(out->session_id, "session");
   return mode == 3 ? HOST_TEXT_REFUSED : HOST_TEXT_OK;
}
static host_text_status transform(const host_text_origin *origin, const char *raw, size_t n,
                                  char *out, size_t cap, size_t *len, char *side, size_t sidecap,
                                  size_t *sidelen, void *state)
{
   (void)origin;
   (void)raw;
   (void)n;
   (void)state;
   transforms++;
   assert(cap == HOST_TEXT_MAX_OUTPUT + 1 && sidecap == HOST_TEXT_MAX_SIDECARS + 1);
   memcpy(out, "safe", 4);
   *len = 4;
   *sidelen = 0;
   if (mode == 5)
   {
      out[0] = (char)0xff;
      *len = 1;
   }
   if (mode == 6)
      *len = cap;
   if (mode == 7)
   {
      memcpy(side, "opaque", 6);
      *sidelen = 6;
   }
   return HOST_TEXT_OK;
}
int main(void)
{
   for (mode = 0; mode < 8; mode++)
   {
      pid_t pid = fork();
      assert(pid >= 0);
      if (!pid)
      {
         host_text_owned out = {0};
         if (mode == 0)
         {
            assert(host_text_apply(HOST_TEXT_PLAIN_PROMPT, "\xff", HOST_TEXT_MAX_INPUT + 1, &out) ==
                   HOST_TEXT_PASSTHROUGH);
            assert(!resolves && !transforms && !out.text);
            assert(host_text_install(1, resolve, transform, NULL) == -1);
         }
         else if (mode == 1)
         {
            assert(!host_text_install(1, NULL, NULL, NULL));
            assert(host_text_apply(HOST_TEXT_PLAIN_PROMPT, "x", 1, &out) == HOST_TEXT_UNAVAILABLE);
         }
         else
         {
            assert(!host_text_install(1, resolve, transform, NULL));
            host_text_status rc = host_text_apply(
                mode == 4 ? HOST_TEXT_EXTERNAL_TRANSCRIPT : HOST_TEXT_PLAIN_PROMPT, "x", 1, &out);
            if (mode == 3)
               assert(rc == HOST_TEXT_REFUSED && !transforms);
            else if (mode == 4)
               assert(rc == HOST_TEXT_UNSUPPORTED && !transforms);
            else if (mode == 5 || mode == 6)
               assert(rc == HOST_TEXT_INVALID && !out.text && !out.sidecars_json);
            else
            {
               assert(rc == HOST_TEXT_OK && out.text_len == 4 && !memcmp(out.text, "safe", 4));
               assert(out.sidecars_len == (mode == 7 ? 6u : 0u));
               host_text_destroy(&out);
               assert(!out.text && !out.sidecars_json && !out.text_len);
            }
            assert(host_text_install(1, resolve, transform, NULL) == -1);
         }
         _exit(0);
      }
      int status;
      assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
   }
   puts("PASS8 generic hook feature-off/required/owned-buffer/transcript boundaries; no real "
        "origin resolver installed");
   return 0;
}
