/* Stub for learning_implicit.c in test binaries that link dogfood.o or
 * workflow_learn.o but do not test the implicit-signal detection path.
 * All functions are no-ops; the full implementation lives in learning_implicit.c. */
#include <stdint.h>

void learning_implicit_record_repeat_question(const char *s, const char *tool, const char *q)
{
   (void)s;
   (void)tool;
   (void)q;
}
void learning_implicit_record_correction(const char *k, int64_t id)
{
   (void)k;
   (void)id;
}
void learning_implicit_record_workflow(const char *ws, const char *st, const char *d)
{
   (void)ws;
   (void)st;
   (void)d;
}
