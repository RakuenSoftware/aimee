#include <stdio.h>
#include <string.h>
#include "model_registry.h"
static void show(const char *p, const char *m)
{
   model_capability_t c; memset(&c,0,sizeof(c));
   int rc = model_capability_get(p, m, &c);
   printf("%-10s %-22s rc=%d ctx=%-9d maxout=%-7d in=$%-6.2f %s%s%s\n", p, m, rc,
          c.context_window, c.max_output, c.cost_in_per_mtok,
          (c.flags&MODEL_CAP_REASONING)?"R":"-", (c.flags&MODEL_CAP_TOOLS)?"T":"-",
          (c.flags&MODEL_CAP_VISION)?"V":"-");
}
int main(void){
   show("claude","claude-opus-4-8");     /* live agent's provider string */
   show("anthropic","claude-opus-4-8");  /* correct catalog vendor */
   show("chatgpt","gpt-5.6-sol");        /* live agent's provider string */
   show("openai","gpt-5.6-sol");         /* correct catalog vendor */
   return 0;
}
