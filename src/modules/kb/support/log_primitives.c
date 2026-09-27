#include "kb_store_log.h"

#include <stdio.h>

#define KB_STORE_LOG_MESSAGE_CAP 1024

static kb_store_log_sink_fn s_sink = NULL;
static void *s_context = NULL;

void kb_store_log_install(kb_store_log_sink_fn sink, void *context)
{
   s_context = context;
   s_sink = sink;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   if (!s_sink || !module || !fmt || level < LOG_ERROR || level > LOG_DEBUG)
      return;

   char message[KB_STORE_LOG_MESSAGE_CAP];
   va_list args;
   va_start(args, fmt);
   int written = vsnprintf(message, sizeof(message), fmt, args);
   va_end(args);
   if (written < 0)
      return;
   message[sizeof(message) - 1] = '\0';
   s_sink(s_context, level, module, message);
}
