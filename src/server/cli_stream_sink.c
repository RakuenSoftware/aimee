/* cli_stream_sink.c — thread-local streaming sink for local-CLI agent output.
 * See cli_stream_sink.h. */
#include "cli_stream_sink.h"

static __thread cli_stream_emit_fn t_emit = NULL;
static __thread void *t_ctx = NULL;

void cli_stream_sink_set(cli_stream_emit_fn fn, void *ctx)
{
   t_emit = fn;
   t_ctx = ctx;
}

void cli_stream_sink_clear(void)
{
   t_emit = NULL;
   t_ctx = NULL;
}

int cli_stream_sink_active(void)
{
   return t_emit != NULL;
}

void cli_stream_sink_emit(const char *text, size_t len)
{
   if (t_emit && text && len > 0)
      t_emit(t_ctx, text, len);
}
