#include <stdio.h>
#include <string.h>
#include "model_registry.h"
static void show(const char *p, const char *m)
{
   model_capability_t c; memset(&c,0,sizeof(c));
   if (!model_capability_get(p,m,&c)) { printf("MISS %s/%s\n",p,m); return; }
   printf("%-11s %-20s in=$%-6.2f out=$%-6.2f cached=$%-6.3f %s\n", p, m,
          c.cost_in_per_mtok, c.cost_out_per_mtok, c.cost_cache_read_per_mtok,
          c.display_name[0]?c.display_name:"");
}
int main(void){
   show("minimax","MiniMax-M3"); show("moonshotai","kimi-k2.7-code");
   show("anthropic","claude-opus-4-8"); show("anthropic","claude-haiku-4-5");
   show("openai","gpt-5.6-sol"); show("openai","gpt-5.6-luna");
   return 0;
}
