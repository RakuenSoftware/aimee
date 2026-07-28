/* pam_auth.c — the PAM credential check, and nothing else.
 *
 * MOVED VERBATIM out of posix/dashboard.c, which is where it has lived and been
 * in production. It is extracted rather than copied because there must stay
 * exactly ONE PAM policy in this codebase: the aimee-kb password-login route
 * needs it, kb does not link the dashboard, and the alternative — a second PAM
 * stack for kb — would mean two places deciding what counts as a valid host
 * login, which is precisely the duplication worth avoiding.
 *
 * There is deliberately no aimee-specific policy here. The "aimee" PAM service
 * file decides (which accounts, which factors, lockout, expiry); this asks and
 * reports yes or no.
 */
#include "pam_auth.h"

#include <stdlib.h>
#include <string.h>

#if defined(__linux__) && defined(WITH_PAM)
#include <security/pam_appl.h>

static int pam_conversation(int num_msg, const struct pam_message **msg, struct pam_response **resp,
                            void *appdata_ptr)
{
   const char *password = (const char *)appdata_ptr;
   struct pam_response *reply = calloc((size_t)num_msg, sizeof(struct pam_response));
   if (!reply)
      return PAM_BUF_ERR;
   for (int i = 0; i < num_msg; i++)
   {
      if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF || msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
         reply[i].resp = strdup(password);
   }
   *resp = reply;
   return PAM_SUCCESS;
}

int pam_check_credentials(const char *user, const char *password)
{
   struct pam_conv conv = {pam_conversation, (void *)password};
   pam_handle_t *pamh = NULL;
   int rc = pam_start("aimee", user, &conv, &pamh);
   if (rc != PAM_SUCCESS)
      return 0;
   rc = pam_authenticate(pamh, PAM_SILENT);
   int ok = (rc == PAM_SUCCESS);
   if (ok)
      rc = pam_acct_mgmt(pamh, PAM_SILENT);
   ok = ok && (rc == PAM_SUCCESS);
   pam_end(pamh, rc);
   return ok;
}
#else
int pam_check_credentials(const char *user, const char *password)
{
   (void)user;
   (void)password;
   return 0; /* PAM not available -- reject all credentials */
}
#endif
