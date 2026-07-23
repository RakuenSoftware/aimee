/* Integration check: resolve LIVE fleet models against the REAL models.dev
 * api.json, not a synthetic fixture. Prints what the catalog actually yields. */
#include <stdio.h>
#include <string.h>
#include "model_registry.h"
#include "models_dev.h"

static void show(const char *provider, const char *model)
{
   model_capability_t c;
   memset(&c, 0, sizeof(c));
   int rc = models_dev_cache_lookup(provider, model, &c);
   if (!rc)
   {
      printf("MISS  %s/%s\n", provider, model);
      return;
   }
   printf("HIT   %-34s ctx=%-9d out=%-7d in=$%-6.2f out=$%-6.2f %s%s%s name=%s\n", model,
          c.context_window, c.max_output, c.cost_in_per_mtok, c.cost_out_per_mtok,
          (c.flags & MODEL_CAP_REASONING) ? "R" : "-", (c.flags & MODEL_CAP_TOOLS) ? "T" : "-",
          (c.flags & MODEL_CAP_VISION) ? "V" : "-", c.display_name[0] ? c.display_name : "(none)");
}

int main(void)
{
   show("minimax", "MiniMax-M3");
   show("moonshotai", "kimi-k2.7-code");
   show("anthropic", "claude-opus-4-8");
   show("anthropic", "claude-sonnet-5");
   show("anthropic", "claude-haiku-4-5");
   show("anthropic", "claude-fable-5");
   show("openai", "gpt-5.6-sol");
   show("openai", "gpt-5.6-terra");
   show("openai", "gpt-5.6-luna");
   show("minimax", "MiniMax-M99-does-not-exist");
   return 0;
}
