#ifndef AIMEE_SERVER_NATIVE_SDK_PROFILE_H
#define AIMEE_SERVER_NATIVE_SDK_PROFILE_H
#include <aimee/ir/aimee_ir.h>
#include <stdlib.h>
#include <string.h>

/* The held Claude scheduler uses noninteractive print mode without an appended
 * system prompt. Its pinned protocol prepends this component identity even to
 * custom system prompts. Represent it as host-owned typed context before the
 * runtime/profile/cache copies, never by adopting incoming SDK system bytes.
 * Caller must first obtain the measured descriptor with native_launch_open. */
#define AIMEE_NATIVE_CLAUDE_IDENTITY                                                               \
   "You are a Claude agent, built on Anthropic's Claude Agent SDK."
static inline int server_native_sdk_profile_prepare(aimee_request_t *profile, const char *sdk,
                                                    int executable_fd)
{
   if (!profile || !sdk || executable_fd < 0)
      return -1;
   if (strcmp(sdk, "claude"))
      return strcmp(sdk, "codex") ? -1 : 0;
   if (profile->n_system < 0 || profile->n_system >= AIMEE_IR_MAX_BLOCKS ||
       (profile->n_system && !profile->system))
      return -1;
   aimee_block_t *blocks = calloc((size_t)profile->n_system + 1, sizeof *blocks);
   if (!blocks)
      return -1;
   blocks[0].type = AIMEE_BLK_TEXT;
   aimee_ir_block_set_context(blocks, AIMEE_CTX_ORIGIN_PLATFORM, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_VERIFIED, AIMEE_CTX_SENS_PUBLIC, 1,
                              "native-sdk-profile", "claude", 0);
   blocks[0].text = strdup(AIMEE_NATIVE_CLAUDE_IDENTITY);
   if (!blocks[0].text)
   {
      free(blocks);
      return -1;
   }
   if (profile->n_system)
      memcpy(blocks + 1, profile->system, (size_t)profile->n_system * sizeof *blocks);
   free(profile->system);
   profile->system = blocks;
   profile->n_system++;
   profile->mutated = 1;
   return 0;
}
#endif
