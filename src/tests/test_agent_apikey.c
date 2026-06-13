/* test_agent_apikey.c: agents.json must never persist a resolved secret.
 *
 * Split out of test_agent.c (2000-line hard limit), mirroring its link line.
 *
 * A "$VAR" api_key is a reference, not a secret: it is resolved into the runtime
 * agent_t.api_key at load, but the verbatim reference is preserved in
 * api_key_disk so a save re-serializes the reference, not the expanded value.
 * Without this, any agent-config mutation (add/enable/remove) would rewrite the
 * loaded secret back into agents.json as plaintext. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "aimee.h"
#include "agent.h"
#include "agent_config.h"
#include "config.h"
#include "platform_path.h"

static void test_apikey_ref_not_serialized(void)
{
   const char *cfg_dir = config_default_dir();
   assert(platform_mkdir_p(cfg_dir, 0700) == 0 || access(cfg_dir, F_OK) == 0);

   setenv("AIMEE_APIKEY_REF_TEST", "sk-super-secret-value", 1);

   {
      FILE *f = fopen(agent_config_path(), "w");
      assert(f != NULL);
      fputs("{\"agents\":[{\"name\":\"reftest\",\"endpoint\":\"https://api.example/v1\","
            "\"model\":\"m\",\"roles\":[\"code\"],"
            "\"api_key\":\"$AIMEE_APIKEY_REF_TEST\"}]}\n",
            f);
      fclose(f);
   }

   agent_config_t loaded;
   assert(agent_load_config(&loaded) == 0);
   agent_t *ag = agent_find(&loaded, "reftest");
   assert(ag != NULL);
   assert(strcmp(ag->api_key, "sk-super-secret-value") == 0);       /* resolved at runtime */
   assert(strcmp(ag->api_key_disk, "$AIMEE_APIKEY_REF_TEST") == 0); /* reference preserved */

   /* Save, then read the raw file: it must keep the $VAR ref, not the secret. */
   assert(agent_save_config(&loaded) == 0);
   {
      FILE *f = fopen(agent_config_path(), "r");
      assert(f != NULL);
      char buf[8192];
      size_t n = fread(buf, 1, sizeof(buf) - 1, f);
      fclose(f);
      buf[n] = '\0';
      assert(strstr(buf, "$AIMEE_APIKEY_REF_TEST") != NULL); /* reference written */
      assert(strstr(buf, "sk-super-secret-value") == NULL);  /* secret NOT written */
   }

   /* Reload still resolves to the secret. */
   agent_config_t reloaded;
   assert(agent_load_config(&reloaded) == 0);
   agent_t *ag2 = agent_find(&reloaded, "reftest");
   assert(ag2 != NULL && strcmp(ag2->api_key, "sk-super-secret-value") == 0);

   unsetenv("AIMEE_APIKEY_REF_TEST");
   printf("  PASS: test_apikey_ref_not_serialized\n");
}

int main(void)
{
   char tmp_template[] = "/tmp/aimee-agent-apikey-XXXXXX";
   char *tmp_home = mkdtemp(tmp_template);
   assert(tmp_home != NULL);
   setenv("AIMEE_HOME", tmp_home, 1);

   test_apikey_ref_not_serialized();

   printf("agent_apikey: all tests passed\n");
   return 0;
}
