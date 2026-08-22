/* computer_use.h: guarded policy helpers for external computer-use MCP tools. */
#ifndef DEC_COMPUTER_USE_H
#define DEC_COMPUTER_USE_H 1

#include <stddef.h>

#define COMPUTER_USE_MAX_ALLOWED_DOMAINS 16
#define COMPUTER_USE_DOMAIN_MAX          128

typedef enum
{
   COMPUTER_USE_DECISION_ALLOW = 0,
   COMPUTER_USE_DECISION_APPROVE = 1,
   COMPUTER_USE_DECISION_BLOCK = 2
} computer_use_decision_t;

typedef struct
{
   int enabled;
   char default_navigation[16]; /* approve | block | allow */
   int redact_sensitive_screenshots;
   char allowed_domains[COMPUTER_USE_MAX_ALLOWED_DOMAINS][COMPUTER_USE_DOMAIN_MAX];
   int allowed_domain_count;
} computer_use_policy_t;

/* Reads the live config through the config accessors; callers never hold a legacy_config_record. */
void computer_use_policy_from_config(computer_use_policy_t *out);
int computer_use_is_tool_name(const char *qualified_tool_name);
int computer_use_classify(const computer_use_policy_t *policy, const char *qualified_tool_name,
                          const char *args_json, computer_use_decision_t *decision_out,
                          char *reason_out, size_t reason_len);
int computer_use_redact_text(const char *input, char *out, size_t out_len);

#endif /* DEC_COMPUTER_USE_H */
