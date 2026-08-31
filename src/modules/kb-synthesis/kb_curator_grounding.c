#include "kb_curator_grounding.h"

#include "cJSON.h"

#include <string.h>

static kb_curator_grounding_provider_fn g_grounding_provider;

void kb_curator_grounding_register_provider(kb_curator_grounding_provider_fn provider)
{
   g_grounding_provider = provider;
}

static int claim_from_payload(const struct cJSON *payload, aimee_kb_synthesis_claim_kind_t *kind,
                              const char *claims[AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX],
                              uint32_t *claim_count)
{
   if (!kind || !claims || !claim_count)
      return -1;

   *claim_count = 0;
   const cJSON *side_effects =
       payload ? cJSON_GetObjectItemCaseSensitive(payload, "side_effects") : NULL;
   if (!side_effects || cJSON_IsNull(side_effects))
   {
      *kind = AIMEE_KB_SYNTHESIS_CLAIM_NONE;
      return 0;
   }
   if (cJSON_IsString(side_effects))
   {
      if (!side_effects->valuestring ||
          strlen(side_effects->valuestring) > AIMEE_KB_SYNTHESIS_TEXT_MAX)
         return -1;
      *kind = AIMEE_KB_SYNTHESIS_CLAIM_STRING;
      claims[0] = side_effects->valuestring;
      *claim_count = 1;
      return 0;
   }
   if (!cJSON_IsArray(side_effects))
   {
      *kind = AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING;
      return 0;
   }

   int count = cJSON_GetArraySize(side_effects);
   if (count < 0 || count > (int)AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX)
      return -1;
   *kind = AIMEE_KB_SYNTHESIS_CLAIM_STRING_ARRAY;
   const cJSON *item = NULL;
   cJSON_ArrayForEach(item, side_effects)
   {
      if (!cJSON_IsString(item))
      {
         *kind = AIMEE_KB_SYNTHESIS_CLAIM_NONSTRING;
         *claim_count = 0;
         return 0;
      }
      if (!item->valuestring || strlen(item->valuestring) > AIMEE_KB_SYNTHESIS_TEXT_MAX)
         return -1;
      claims[*claim_count] = item->valuestring;
      (*claim_count)++;
   }
   return 0;
}

int kb_curator_grounding_decide(const struct cJSON *payload, const char *const *callees,
                                uint32_t callee_count,
                                aimee_kb_synthesis_grounding_decision_t *decision)
{
   if (!decision || callee_count > AIMEE_KB_SYNTHESIS_CALLEE_COUNT_MAX ||
       (callee_count > 0 && !callees) || !g_grounding_provider)
      return -1;

   const char *claims[AIMEE_KB_SYNTHESIS_CLAIM_COUNT_MAX] = {0};
   uint32_t claim_count = 0;
   aimee_kb_synthesis_claim_kind_t kind;
   if (claim_from_payload(payload, &kind, claims, &claim_count) != 0)
      return -1;

   memset(decision, 0, sizeof *decision);
   if (g_grounding_provider(kind, claims, claim_count, callees, callee_count, decision) != 0 ||
       !memchr(decision->reason, '\0', sizeof(decision->reason)) ||
       (decision->contradicts != 0 && decision->contradicts != 1) ||
       ((decision->contradicts != 0) != (decision->reason[0] != '\0')))
   {
      memset(decision, 0, sizeof *decision);
      return -1;
   }
   return 0;
}
