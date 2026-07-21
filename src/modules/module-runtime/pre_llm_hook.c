/* pre_llm_hook.c: required in-process PRE_LLM_CALL hook registry.
 *
 * Part of the pluggable context engine surface described in
 * docs/proposals/pending/plugin-extension-surface-and-context-engine.md.
 */
#include "aimee/module-runtime/pre_llm_hook.h"
#include "headers/log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- PRE_LLM_CALL registry --- */

typedef struct
{
   plugin_chook_pre_llm_fn fn;
   void *user_data;
} pre_llm_entry_t;

static pre_llm_entry_t g_pre_llm[PLUGIN_C_HOOK_MAX];
static int g_pre_llm_count = 0;

int plugin_chook_register_pre_llm(plugin_chook_pre_llm_fn fn, void *user_data)
{
   if (!fn)
   {
      LOG_WARN("plugin_c_hook", "register_pre_llm: NULL function pointer rejected");
      return -1;
   }
   if (g_pre_llm_count >= PLUGIN_C_HOOK_MAX)
   {
      LOG_WARN("plugin_c_hook", "register_pre_llm: registry full (%d hooks)", PLUGIN_C_HOOK_MAX);
      return -1;
   }
   g_pre_llm[g_pre_llm_count].fn = fn;
   g_pre_llm[g_pre_llm_count].user_data = user_data;
   g_pre_llm_count++;
   LOG_DEBUG("plugin_c_hook", "pre_llm hook registered (%d total)", g_pre_llm_count);
   return 0;
}

int plugin_chook_run_pre_llm(const char *user_msg, char *ephemeral_out, size_t out_len)
{
   if (!ephemeral_out || out_len == 0)
      return 0;

   ephemeral_out[0] = '\0';
   size_t pos = 0;
   int included = 0;

   char hook_buf[2048];
   for (int i = 0; i < g_pre_llm_count; i++)
   {
      hook_buf[0] = '\0';
      int rc = g_pre_llm[i].fn(user_msg, hook_buf, sizeof(hook_buf), g_pre_llm[i].user_data);
      if (rc != 0 || !hook_buf[0])
         continue;

      size_t hook_len = strlen(hook_buf);

      /* Append separator if not first */
      if (pos > 0 && pos + 1 < out_len)
         ephemeral_out[pos++] = '\n';

      /* Copy hook output, truncating if necessary */
      size_t copy_len = hook_len;
      if (pos + copy_len >= out_len)
         copy_len = out_len - pos - 1;
      if (copy_len == 0)
         break;

      memcpy(ephemeral_out + pos, hook_buf, copy_len);
      pos += copy_len;
      ephemeral_out[pos] = '\0';
      included++;
   }

   return included;
}

char *plugin_chook_apply_pre_llm(const char *user_msg)
{
   char ephemeral[4096] = {0};
   if (plugin_chook_run_pre_llm(user_msg, ephemeral, sizeof(ephemeral)) <= 0 || !ephemeral[0])
      return NULL;
   size_t up_len = user_msg ? strlen(user_msg) : 0;
   size_t ec_len = strlen(ephemeral);
   char *aug = malloc(up_len + 1 + ec_len + 1);
   if (!aug)
      return NULL;
   if (up_len)
      memcpy(aug, user_msg, up_len);
   aug[up_len] = '\n';
   memcpy(aug + up_len + 1, ephemeral, ec_len + 1);
   return aug;
}

void plugin_chook_reset(void)
{
   memset(g_pre_llm, 0, sizeof(g_pre_llm));
   g_pre_llm_count = 0;
}

int plugin_chook_pre_llm_count(void)
{
   return g_pre_llm_count;
}
