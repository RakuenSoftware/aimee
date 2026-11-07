#ifndef AIMEE_TEST_IR_RESPONSE_FIXTURE_H
#define AIMEE_TEST_IR_RESPONSE_FIXTURE_H
#include <aimee/ir/aimee_ir.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Authored fixture output, never a native decoder or serving fallback. */
static inline int fixture_ir_response_from_text(aimee_response_t *out, const char *text,
                                                const char *model)
{
   if (!out)
      return -1;
   *out = (aimee_response_t){0};
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)(text ? text : "")};
   aimee_response_t response = {.role = "assistant",
                                .content = &block,
                                .n_content = 1,
                                .stop_reason = AIMEE_STOP_END_TURN,
                                .raw_stop_reason = "end_turn",
                                .model = (char *)(model && model[0] ? model : NULL)};
   return aimee_ir_response_clone(&response, out);
}

static inline void assert_ir_text(const aimee_response_t *response, const char *expected)
{
   char *text = aimee_ir_response_text_alloc(response);
   assert(text && !strcmp(text, expected));
   free(text);
}
#endif
