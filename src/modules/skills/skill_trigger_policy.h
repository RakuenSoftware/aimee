#ifndef AIMEE_SKILL_TRIGGER_POLICY_H
#define AIMEE_SKILL_TRIGGER_POLICY_H 1

typedef int (*skill_trigger_match_provider_fn)(const char *content, const char *tool_name,
                                               const char *subject, int *match);

/* Production installs the event-bus provider during server startup. NULL clears it. */
void skill_trigger_register_match_provider(skill_trigger_match_provider_fn provider);

/* Pure process-side policy used by the C parity handler. */
int skill_trigger_policy_matches_content(const char *content, const char *tool_name,
                                         const char *subject);

#endif
