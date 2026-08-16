#include "kb_http_jobs.h"
#include "modules/db2/c/kb_service_backend.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int jobs_json_escape(const char *s, char *out, int pos, int cap)
{
   for (; s && *s && pos + 2 < cap; s++)
   {
      if (*s == '"' || *s == '\\')
         out[pos++] = '\\';
      out[pos++] = *s;
   }
   if (pos < cap)
      out[pos] = '\0';
   return pos;
}

int handle_get_job_status(const char *path, char *out_buf, int out_cap)
{
   const char *id_s = path ? path + 9 : "";
   if (!id_s || !*id_s)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }
   errno = 0;
   char *end = NULL;
   long long parsed = strtoll(id_s, &end, 10);
   if (errno || !end || *end || parsed <= 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }

   db2_kb_service_async_job_t job;
   int found = db2_kb_service_async_job_get((int64_t)parsed, &job);
   if (found < 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"job status unavailable\"}");
      return 503;
   }
   if (found == 0)
   {
      snprintf(out_buf, (size_t)out_cap, "{\"error\":\"not found\"}");
      return 404;
   }

   int pos = snprintf(out_buf, (size_t)out_cap, "{\"id\":%lld,\"kind\":\"", (long long)job.id);
   pos = jobs_json_escape(job.kind, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"document_id\":%lld,\"project\":\"",
                   (long long)job.document_id);
   pos = jobs_json_escape(job.project, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"status\":\"");
   pos = jobs_json_escape(job.status, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"attempts\":%d,\"last_error\":\"",
                   job.attempts);
   pos = jobs_json_escape(job.last_error, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"claimed_by\":\"");
   pos = jobs_json_escape(job.claimed_by, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"claimed_at\":\"");
   pos = jobs_json_escape(job.claimed_at, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"created_at\":\"");
   pos = jobs_json_escape(job.created_at, out_buf, pos, out_cap);
   pos += snprintf(out_buf + pos, (size_t)(out_cap - pos), "\",\"updated_at\":\"");
   pos = jobs_json_escape(job.updated_at, out_buf, pos, out_cap);
   snprintf(out_buf + pos, (size_t)(out_cap - pos), "\"}");
   return 200;
}
