#include "db2_log.h"

#include <stdio.h>

#define DB2_LOG_MESSAGE_CAP 1024

static db2_log_sink_fn s_sink = NULL;
static void *s_context = NULL;

void db2_log_install(db2_log_sink_fn sink, void *context)
{
   s_context = context;
   s_sink = sink;
}

void aimee_log(log_level_t level, const char *module, const char *fmt, ...)
{
   if (!s_sink || !module || !fmt || level < LOG_ERROR || level > LOG_DEBUG)
      return;

   char message[DB2_LOG_MESSAGE_CAP];
   va_list args;
   va_start(args, fmt);
   int written = vsnprintf(message, sizeof(message), fmt, args);
   va_end(args);
   if (written < 0)
      return;
   message[sizeof(message) - 1] = '\0';
   s_sink(s_context, level, module, message);
}
