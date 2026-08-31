/* posix/agent_runtime_support.c: smaller POSIX support pieces for agent runtime. */

#include "aimee.h"
#include "agent.h"
#include "agent_eval.h"
#include "cJSON.h"
#include "log.h"
#include "util.h"
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int agent_ssh_setup(const agent_network_t *network, char *key_path_out, size_t key_path_len,
                    char *session_id_out, size_t session_id_len)
{
   /* Generate session ID */
   snprintf(session_id_out, session_id_len, "aimee-session-%d", (int)getpid());

   /* Create temp directory */
   char tmpdir[] = "/tmp/aimee-agent-XXXXXX";
   if (!mkdtemp(tmpdir))
      return -1;

   /* Generate ephemeral Ed25519 key pair */
   char key_file[MAX_PATH_LEN];
   snprintf(key_file, sizeof(key_file), "%s/id_ed25519", tmpdir);
   snprintf(key_path_out, key_path_len, "%s", key_file);

   const char *keygen_argv[] = {"ssh-keygen", "-t", "ed25519",      "-f", key_file, "-N",
                                "",           "-C", session_id_out, "-q", NULL};
   char *keygen_out = NULL;
   if (safe_exec_capture(keygen_argv, &keygen_out, 4096) != 0)
   {
      free(keygen_out);
      return -1;
   }
   free(keygen_out);

   /* Read the public key */
   char pub_file[MAX_PATH_LEN];
   snprintf(pub_file, sizeof(pub_file), "%s.pub", key_file);
   FILE *f = fopen(pub_file, "r");
   if (!f)
      return -1;
   char pubkey[4096];
   pubkey[0] = '\0';
   if (fgets(pubkey, sizeof(pubkey), f))
   {
      size_t len = strlen(pubkey);
      while (len > 0 && (pubkey[len - 1] == '\n' || pubkey[len - 1] == '\r'))
         pubkey[--len] = '\0';
   }
   fclose(f);

   if (!pubkey[0])
      return -1;

   /* Authorize the key on the deploy host via existing SSH access.
    * Parse the deploy host from ssh_entry (e.g., "ssh -p 2222 deploy@host") */
   {
      char *safe_pubkey = shell_quote(pubkey);
      char auth_script[8192];
      snprintf(auth_script, sizeof(auth_script), "printf '%%s\\n' %s >> ~/.ssh/authorized_keys",
               safe_pubkey);
      free(safe_pubkey);
      char *ssh_tokens[32];
      int stc = shlex_split(network->ssh_entry, ssh_tokens, 30);
      if (stc <= 0)
      {
         const char *rm_argv[] = {"rm", "-rf", tmpdir, NULL};
         char *rm_out = NULL;
         safe_exec_capture(rm_argv, &rm_out, 256);
         free(rm_out);
         return -1;
      }
      const char *ssh_argv[34];
      for (int si = 0; si < stc && si < 30; si++)
         ssh_argv[si] = ssh_tokens[si];
      ssh_argv[stc] = auth_script;
      ssh_argv[stc + 1] = NULL;
      char *ssh_out = NULL;
      int ssh_rc = safe_exec_capture(ssh_argv, &ssh_out, 4096);
      free(ssh_out);
      for (int si = 0; si < stc; si++)
         free(ssh_tokens[si]);
      if (ssh_rc != 0)
      {
         const char *rm_argv[] = {"rm", "-rf", tmpdir, NULL};
         char *rm_out = NULL;
         safe_exec_capture(rm_argv, &rm_out, 256);
         free(rm_out);
         return -1;
      }
   }

   return 0;
}

void agent_ssh_cleanup(const agent_network_t *network, const char *key_path, const char *session_id)
{
   if (!key_path || !key_path[0])
      return;

   /* Remove the ephemeral key from deploy's authorized_keys */
   if (network && network->ssh_entry[0] && session_id && session_id[0])
   {
      /* Sanitize session_id to prevent sed/shell injection: only allow
       * alphanumeric chars and hyphens in the pattern */
      char safe_sid[128];
      size_t si = 0;
      for (size_t k = 0; session_id[k] && si < sizeof(safe_sid) - 1; k++)
      {
         char c = session_id[k];
         if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '-' || c == '_')
            safe_sid[si++] = c;
      }
      safe_sid[si] = '\0';
      if (!safe_sid[0])
         return; /* nothing safe to match on */
      char sed_script[512];
      snprintf(sed_script, sizeof(sed_script), "sed -i '/%s/d' ~/.ssh/authorized_keys", safe_sid);
      char *ssh_tokens[32];
      int stc = shlex_split(network->ssh_entry, ssh_tokens, 30);
      if (stc > 0)
      {
         const char *ssh_argv[34];
         for (int si = 0; si < stc && si < 30; si++)
            ssh_argv[si] = ssh_tokens[si];
         ssh_argv[stc] = sed_script;
         ssh_argv[stc + 1] = NULL;
         char *ssh_out = NULL;
         int rc = safe_exec_capture(ssh_argv, &ssh_out, 256);
         free(ssh_out);
         if (rc != 0)
            aimee_log(LOG_WARN, "agent_context", "failed to revoke ephemeral key");
         for (int si = 0; si < stc; si++)
            free(ssh_tokens[si]);
      }
   }

   /* Delete the local key pair directory */
   char dir[MAX_PATH_LEN];
   snprintf(dir, sizeof(dir), "%s", key_path);
   /* key_path is like /tmp/aimee-agent-XXXXXX/id_ed25519, get the dir */
   char *slash = strrchr(dir, '/');
   if (slash)
   {
      *slash = '\0';
      const char *rm_argv[] = {"rm", "-rf", dir, NULL};
      char *rm_out = NULL;
      safe_exec_capture(rm_argv, &rm_out, 256);
      free(rm_out);
   }
}

int agent_eval_load_tasks(const char *suite_dir, eval_task_t *tasks, int max_tasks)
{
   if (!suite_dir || !tasks)
      return 0;

   /* Scan directory for *.json files */
   char pattern[MAX_PATH_LEN];
   snprintf(pattern, sizeof(pattern), "%s/*.json", suite_dir);

   glob_t gl;
   if (glob(pattern, 0, NULL, &gl) != 0)
      return 0;

   int count = 0;
   for (size_t i = 0; i < gl.gl_pathc && count < max_tasks; i++)
   {
      FILE *f = fopen(gl.gl_pathv[i], "r");
      if (!f)
         continue;
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz <= 0 || sz > 65536)
      {
         fclose(f);
         continue;
      }
      char *data = malloc((size_t)sz + 1);
      size_t nread = fread(data, 1, (size_t)sz, f);
      data[nread] = '\0';
      fclose(f);

      cJSON *root = cJSON_Parse(data);
      free(data);
      if (!root)
         continue;

      eval_task_t *t = &tasks[count];
      memset(t, 0, sizeof(*t));

      cJSON *name = cJSON_GetObjectItem(root, "name");
      cJSON *prompt = cJSON_GetObjectItem(root, "prompt");
      cJSON *role = cJSON_GetObjectItem(root, "role");

      if (name && cJSON_IsString(name))
         snprintf(t->name, sizeof(t->name), "%s", name->valuestring);
      if (prompt && cJSON_IsString(prompt))
         snprintf(t->prompt, sizeof(t->prompt), "%s", prompt->valuestring);
      if (role && cJSON_IsString(role))
         snprintf(t->role, sizeof(t->role), "%s", role->valuestring);
      else
         snprintf(t->role, sizeof(t->role), "execute");

      cJSON *sc = cJSON_GetObjectItem(root, "success_check");
      if (sc)
      {
         cJSON *sctype = cJSON_GetObjectItem(sc, "type");
         cJSON *scval = cJSON_GetObjectItem(sc, "value");
         if (sctype && cJSON_IsString(sctype))
            snprintf(t->success_check_type, sizeof(t->success_check_type), "%s",
                     sctype->valuestring);
         if (scval && cJSON_IsString(scval))
            snprintf(t->success_check_value, sizeof(t->success_check_value), "%s",
                     scval->valuestring);
      }

      cJSON *mt = cJSON_GetObjectItem(root, "max_turns");
      t->max_turns = (mt && cJSON_IsNumber(mt)) ? mt->valueint : 10;

      cJSON *ml = cJSON_GetObjectItem(root, "max_latency_ms");
      t->max_latency_ms = (ml && cJSON_IsNumber(ml)) ? ml->valueint : 60000;

      cJSON_Delete(root);
      count++;
   }
   globfree(&gl);
   return count;
}
