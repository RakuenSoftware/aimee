#ifndef AIMEE_SERVER_UNKNOWN_EFFECT_POLICY_H
#define AIMEE_SERVER_UNKNOWN_EFFECT_POLICY_H
#include <stdint.h>
#include <stddef.h>
/* Call only after ordinary policy permits. This is an additional host gate,
 * never a grant and never an inference about an old interrupted effect. */
int server_unknown_effect_policy(const char *tool, const char *arguments, const char *attempt,
                                 uint32_t categories, char *reason, size_t cap);
/* stage2 revokes ephemeral credit without source reads or effects. stage1
 * final_stage consumes only the matching ephemeral initial approval once;
 * changed effective arguments require their own exact permission. */
int server_unknown_effect_policy_stage(const char *tool, const char *arguments, const char *attempt,
                                       uint32_t categories, int final_stage, char *reason,
                                       size_t cap);
#endif
