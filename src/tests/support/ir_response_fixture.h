#ifndef AIMEE_TEST_IR_RESPONSE_FIXTURE_H
#define AIMEE_TEST_IR_RESPONSE_FIXTURE_H
#include <aimee/ir/aimee_ir.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static inline void assert_ir_text(const aimee_response_t *response, const char *expected)
{
   char *text = aimee_ir_response_text_alloc(response);
   assert(text && !strcmp(text, expected));
   free(text);
}
#endif
