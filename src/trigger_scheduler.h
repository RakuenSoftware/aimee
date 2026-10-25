#pragma once

#include <stddef.h>

int cron_response_is_silent(const char *text);
int cron_wake_gate_should_wake(const char *script_output, char *reason, size_t reason_len);
/* Literal, case-sensitive substring gate. Empty needle means no gate. */
int cron_when_context_contains_allows(const char *prior_output, const char *needle);
/* Returns bytes that would be written, snprintf-style; n >= bufsz means truncated. */
int cron_build_context_preamble(char *buf, size_t bufsz, const char *workdir,
                                const char *skills_csv, const char *deliver_target);
void trigger_scheduler_init(void);
void trigger_scheduler_shutdown(void);
