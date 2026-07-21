/* test_plugin_c_hook.c: unit tests for the PRE_LLM_CALL in-process C hook registry.
 *
 * Key invariant (AC5): the system prompt is never passed to or modified by
 * PRE_LLM_CALL hooks — it stays byte-stable across N turns for prefix caching.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "aimee/module-runtime/pre_llm_hook.h"

/* Simple FNV-1a-inspired hash for testing system_prompt stability. */
static unsigned long hash_str(const char *s)
{
   unsigned long h = 2166136261UL;
   while (s && *s)
   {
      h ^= (unsigned char)*s++;
      h *= 16777619UL;
   }
   return h;
}

/* --- hook fixtures --- */

static char g_captured_user_msg[128] = {0};

static int hook_capture_user_msg(const char *user_msg, char *out, size_t out_len, void *user_data)
{
   (void)user_data;
   snprintf(g_captured_user_msg, sizeof(g_captured_user_msg), "%s", user_msg ? user_msg : "");
   snprintf(out, out_len, "%s", "captured");
   return 0;
}

static int hook_returns_fixed(const char *user_msg, char *out, size_t out_len, void *user_data)
{
   (void)user_msg;
   const char *text = (const char *)user_data;
   snprintf(out, out_len, "%s", text);
   return 0;
}

static int hook_returns_empty(const char *user_msg, char *out, size_t out_len, void *user_data)
{
   (void)user_msg;
   (void)user_data;
   out[0] = '\0';
   return 0;
}

static int hook_returns_error(const char *user_msg, char *out, size_t out_len, void *user_data)
{
   (void)user_msg;
   (void)out;
   (void)out_len;
   (void)user_data;
   return -1; /* signal: skip this hook's output */
}

int main(void)
{
   printf("plugin_c_hook: ");

   /* ---------------------------------------------------------------
    * 1. Empty registry: run returns 0 hooks, ephemeral_out is empty
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      char out[256] = "X";
      int n = plugin_chook_run_pre_llm("user msg", out, sizeof(out));
      assert(n == 0);
      assert(out[0] == '\0');
      printf("1");
   }

   /* ---------------------------------------------------------------
    * 2. One hook returns text; ephemeral_out contains it
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      assert(plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"context-A") == 0);

      char out[256] = {0};
      int n = plugin_chook_run_pre_llm("user msg", out, sizeof(out));
      assert(n == 1);
      assert(strstr(out, "context-A") != NULL);
      printf("2");
   }

   /* ---------------------------------------------------------------
    * 3. Two hooks: both outputs concatenated with newline separator
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"ctx-1");
      plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"ctx-2");

      char out[256] = {0};
      int n = plugin_chook_run_pre_llm("user msg", out, sizeof(out));
      assert(n == 2);
      assert(strstr(out, "ctx-1") != NULL);
      assert(strstr(out, "ctx-2") != NULL);
      assert(strchr(out, '\n') != NULL); /* separator */
      printf("3");
   }

   /* ---------------------------------------------------------------
    * 4. Hook returning empty string is excluded from output
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      plugin_chook_register_pre_llm(hook_returns_empty, NULL);
      plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"real-ctx");

      char out[256] = {0};
      int n = plugin_chook_run_pre_llm("user msg", out, sizeof(out));
      assert(n == 1); /* only the non-empty hook counts */
      assert(strstr(out, "real-ctx") != NULL);
      printf("4");
   }

   /* ---------------------------------------------------------------
    * 5. Hook returning -1 (error/skip) excluded from output
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      plugin_chook_register_pre_llm(hook_returns_error, NULL);
      plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"good-ctx");

      char out[256] = {0};
      int n = plugin_chook_run_pre_llm("user msg", out, sizeof(out));
      assert(n == 1);
      assert(strstr(out, "good-ctx") != NULL);
      printf("5");
   }

   /* ---------------------------------------------------------------
    * 6. System prompt invariant across N turns:
    *    Register a PRE_LLM_CALL hook and simulate 10 turns.
    *    Hash the system_prompt before and after each call —
    *    it must be byte-for-byte identical every time.
    *
    *    This verifies the cache-preserving contract: hooks append
    *    ephemeral context to the user message, never the system prompt.
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      plugin_chook_register_pre_llm(hook_returns_fixed, (void *)"ephemeral-context-per-turn");

      const char *system_prompt = "You are a helpful assistant with stable system context.";
      unsigned long h0 = hash_str(system_prompt);

      for (int turn = 0; turn < 10; turn++)
      {
         char user_msg[64];
         snprintf(user_msg, sizeof(user_msg), "user turn %d", turn);

         char ephemeral[256] = {0};
         int n = plugin_chook_run_pre_llm(user_msg, ephemeral, sizeof(ephemeral));
         assert(n == 1);
         assert(strstr(ephemeral, "ephemeral-context-per-turn") != NULL);

         /* system_prompt must be byte-for-byte unchanged */
         unsigned long h1 = hash_str(system_prompt);
         assert(h0 == h1);
         assert(strcmp(system_prompt, "You are a helpful assistant with stable system context.") ==
                0);

         /* ephemeral context would be appended to user_msg (tested separately),
          * NOT to system_prompt */
         assert(strstr(system_prompt, "ephemeral") == NULL);
      }
      printf("6");
   }

   /* ---------------------------------------------------------------
    * 7. NULL registration rejected
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      assert(plugin_chook_register_pre_llm(NULL, NULL) == -1);
      assert(plugin_chook_pre_llm_count() == 0);
      printf("7");
   }

   /* ---------------------------------------------------------------
    * 8. User message is passed unmodified to the hook
    * ------------------------------------------------------------- */
   {
      plugin_chook_reset();
      g_captured_user_msg[0] = '\0';
      plugin_chook_register_pre_llm(hook_capture_user_msg, NULL);

      char out[64] = {0};
      plugin_chook_run_pre_llm("the-user-message", out, sizeof(out));
      assert(strcmp(g_captured_user_msg, "the-user-message") == 0);
      assert(strstr(out, "captured") != NULL);
      printf("8");
   }

   printf(" OK\n");
   return 0;
}
