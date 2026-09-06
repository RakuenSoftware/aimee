/* PII gate connection seam.
 *
 * Every decision is supplied by the Go memory process. Missing or failed
 * providers fail closed; there is no C cue list or sensitivity policy.
 */
#include "memory_pii_gate.h"

static memory_pii_turn_classifier_fn g_turn_classifier;
static memory_pii_sensitivity_batch_fn g_sensitivity_batch;
static memory_pii_inject_classifier_fn g_inject_classifier;

void memory_pii_register_turn_classifier(memory_pii_turn_classifier_fn classifier)
{
   g_turn_classifier = classifier;
}

void memory_pii_register_sensitivity_batch(memory_pii_sensitivity_batch_fn classifier)
{
   g_sensitivity_batch = classifier;
}

void memory_pii_register_inject_classifier(memory_pii_inject_classifier_fn classifier)
{
   g_inject_classifier = classifier;
}

int memory_pii_turn_requests_sensitive(const char *turn_text)
{
   int requested = 0;
   return turn_text && turn_text[0] && g_turn_classifier &&
                  g_turn_classifier(turn_text, &requested) == 0 && requested
              ? 1
              : 0;
}

int memory_pii_rel_sensitivity_batch(const char *const *rel_types, int count,
                                     rel_sensitivity_t *out)
{
   return rel_types && out && count > 0 && g_sensitivity_batch &&
                  g_sensitivity_batch(rel_types, count, out) == 0
              ? 0
              : -1;
}

rel_sensitivity_t memory_pii_rel_sensitivity(const char *rel_type)
{
   const char *input[1] = {rel_type};
   rel_sensitivity_t result = SENS_SECRET;
   return memory_pii_rel_sensitivity_batch(input, 1, &result) == 0 ? result : SENS_SECRET;
}

int memory_pii_should_inject(rel_sensitivity_t sensitivity, double confidence,
                             int turn_requests_sensitive)
{
   int allowed = 0;
   return g_inject_classifier &&
                  g_inject_classifier((int)sensitivity, confidence, turn_requests_sensitive,
                                      &allowed) == 0 &&
                  allowed
              ? 1
              : 0;
}
