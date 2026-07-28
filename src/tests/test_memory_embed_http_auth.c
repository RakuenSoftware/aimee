/* Native KB embed client: managed service auth must propagate and fail closed. */
#include "support/mock_agent_http.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int memory_embed_http_post(const char *base, const char *path, const char *body, char **resp);

static int s_posts;

static int post_ok(const char *url, const char *auth_header, const char *body, char **response_buf,
                   int timeout_ms, const char *extra_headers)
{
   (void)timeout_ms;
   (void)extra_headers;
   s_posts++;
   assert(strcmp(url, "http://aimee-llm:8742/embed") == 0);
   assert(auth_header && strcmp(auth_header, "Authorization: Bearer kb-service-token") == 0);
   assert(strcmp(body, "probe") == 0);
   *response_buf = strdup("[1.0]");
   return 200;
}

int main(void)
{
   char *resp = NULL;
   mock_agent_http_reset();
   mock_agent_http_set_post_handler(post_ok);

   setenv("AIMEE_LLM_AUTH_REQUIRED", "1", 1);
   setenv("AIMEE_LLM_AUTH_TOKEN", "kb-service-token", 1);
   assert(memory_embed_http_post("http://aimee-llm:8742/", "/embed", "probe", &resp) == 0);
   assert(resp && strcmp(resp, "[1.0]") == 0);
   free(resp);
   assert(s_posts == 1);

   /* A managed KB never issues an unauthenticated request. */
   unsetenv("AIMEE_LLM_AUTH_TOKEN");
   resp = NULL;
   assert(memory_embed_http_post("http://aimee-llm:8742", "/embed", "probe", &resp) == -1);
   assert(resp == NULL);
   assert(s_posts == 1);

   /* Header construction must fail rather than truncate an external token. */
   char oversized[700];
   memset(oversized, 'a', sizeof(oversized) - 1);
   oversized[sizeof(oversized) - 1] = '\0';
   setenv("AIMEE_LLM_AUTH_TOKEN", oversized, 1);
   assert(memory_embed_http_post("http://aimee-llm:8742", "/embed", "probe", &resp) == -1);
   assert(s_posts == 1);

   unsetenv("AIMEE_LLM_AUTH_REQUIRED");
   unsetenv("AIMEE_LLM_AUTH_TOKEN");
   mock_agent_http_reset();
   puts("memory-embed-http-auth: bearer propagated; managed missing/oversized token denied");
   return 0;
}
