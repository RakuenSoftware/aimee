/* Native prose dialect parsing belongs to the Go delegates entry adapter. */
#ifndef DEC_DELEGATE_XML_FALLBACK_H
#define DEC_DELEGATE_XML_FALLBACK_H 1
#include <aimee/ir/aimee_ir.h>
/* Owned canonical message, or no match. -1 refuses without dispatch. */
typedef int (*delegate_rescue_provider_fn)(const char *text, int allow_json, int detect_only,
                                           aimee_message_t *out);
void delegate_register_rescue_provider(delegate_rescue_provider_fn provider);
int delegate_rescue_message(const char *text, aimee_message_t *out, int allow_json);
int delegate_rescue_has_tool_calls_with_json(const char *text, int allow_json);
int delegate_rescue_has_tool_calls(const char *text);
#endif
