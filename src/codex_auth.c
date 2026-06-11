/* codex_auth.c: see codex_auth.h. */
#include "codex_auth.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read a small JSON file into a heap buffer (caller frees). NULL on error or if
 * larger than 1 MB (auth.json is a few KB). */
static char *codex_read_file(const char *path)
{
   FILE *f = fopen(path, "rb");
   if (!f)
      return NULL;
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   long sz = ftell(f);
   if (fseek(f, 0, SEEK_SET) != 0 || sz <= 0 || sz > (1 << 20))
   {
      fclose(f);
      return NULL;
   }
   char *buf = (char *)malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t got = fread(buf, 1, (size_t)sz, f);
   fclose(f);
   buf[got] = '\0';
   return buf;
}

int codex_local_auth(char *token, size_t token_len, char *account_id, size_t account_id_len)
{
   if (token && token_len)
      token[0] = '\0';
   if (account_id && account_id_len)
      account_id[0] = '\0';

   const char *home = getenv("HOME");
   if (!home || !home[0])
      return -1;

   char path[1024];
   snprintf(path, sizeof(path), "%s/.codex/auth.json", home);
   char *buf = codex_read_file(path);
   if (!buf)
      return -1;

   cJSON *root = cJSON_Parse(buf);
   free(buf);
   if (!root)
      return -1;

   /* tokens.{access_token,account_id} is the OAuth (chatgpt) shape; the
    * top-level keys are the fallback (matches agent_read_codex_oauth_token). */
   cJSON *access = cJSON_GetObjectItemCaseSensitive(root, "access_token");
   cJSON *acct = cJSON_GetObjectItemCaseSensitive(root, "account_id");
   cJSON *tokens = cJSON_GetObjectItemCaseSensitive(root, "tokens");
   if (cJSON_IsObject(tokens))
   {
      if (!cJSON_IsString(access))
         access = cJSON_GetObjectItemCaseSensitive(tokens, "access_token");
      if (!cJSON_IsString(acct))
         acct = cJSON_GetObjectItemCaseSensitive(tokens, "account_id");
   }

   int ok = 0;
   if (cJSON_IsString(access) && access->valuestring[0])
   {
      if (token && token_len)
         snprintf(token, token_len, "%s", access->valuestring);
      ok = 1;
   }
   if (cJSON_IsString(acct) && acct->valuestring[0] && account_id && account_id_len)
      snprintf(account_id, account_id_len, "%s", acct->valuestring);

   cJSON_Delete(root);
   return ok ? 0 : -1;
}
