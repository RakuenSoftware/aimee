#define DEC_HUD_H 1

#include "aimee.h"
#include "cJSON.h"
#include "guardrails.h"

typedef struct
{
   char mode[64];
   int total_calls;
   int successful_calls;
   int failed_calls;
   long long total_prompt_tokens;
   long long total_completion_tokens;
   long long total_cache_write_tokens;
   long long total_cache_read_tokens;
   double total_estimated_cost_usd; /* legacy: all-rows cost (kept for compat) */
   /* Spend by usage_kind (§7): realized is the billable spend; estimated, avoided
    * (dedup-skipped), and partial (aborted) are reported separately and excluded
    * from the billable figure. */
   double spend_realized_usd;
   double spend_estimated_usd;
   double spend_avoided_usd;
   double spend_partial_usd;
   int total_turns;
   int total_tool_calls;
   double avg_latency_ms;
   int recent_calls;
   int recent_successes;
   int semantic_warn_count;
   /* KB health (best-effort, 200ms timeout; 0 if aimee-kb not running) */
   int kb_health_checked; /* 1 if kb_client_health() returned a result */
   int kb_health_warn;    /* 1 if any check returned warn */
   int kb_health_fail;    /* 1 if any check returned fail/error */
   /* Ensemble (MoA) cost, populated if any ensemble calls were made this session */
   int ensemble_call_count;
   double ensemble_cost_usd;
} hud_status_t;

int hud_gather(hud_status_t *out);
void hud_print(const hud_status_t *s);
char *hud_json(const hud_status_t *s);
