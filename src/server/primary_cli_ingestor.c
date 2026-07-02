/* primary_cli_ingestor.c -- see primary_cli_ingestor.h. */
#include "primary_cli_ingestor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_shell.h"
#include "dstr.h"
#include "wfe_bind_ingress.h"

int primary_cli_ingestor_enabled(void)
{
   const char *v = getenv("AIMEE_PRIMARY_CLI_INGESTOR");
   if (!v || !v[0])
      return 0;
   return strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0;
}

int primary_cli_ingestor_enforce_preturn(const char *session_id, const char *message,
                                         const char *repo)
{
   /* Trust boundary: with no resolvable aimee session id the seam CANNOT enforce.
    * Return 0 (unbound) rather than pretend -- a caller that lost the session id
    * must degrade to a generic turn, never a silent "enforced" that binds nothing
    * and leaves the guard unanchored. */
   if (!session_id || !session_id[0])
      return 0;

   /* wfe_bind_interactive is itself dial-gated (default-off) and only binds an
    * enforced-routed turn (converse/research stay unbound). Calling it HERE --
    * before the turn is dispatched to the CLI -- is what makes S2 preventive for
    * the external-CLI primary, whose out-of-band model call the gateway router
    * never sees with a session id attached. */
   return wfe_bind_interactive(session_id, message, repo);
}

/* Per-turn ingestion accumulator: the assistant text (streamed as TEXT_DELTA),
 * the backend session id (for next-turn resume), the first error, and a native
 * tool-call count (observed for audit only). */
typedef struct
{
   dstr_t text;
   char *session;
   char error[256];
   int tool_calls;
} pci_ingest_ctx_t;

static void pci_ingest_cb(agent_shell_event_t event, const char *data, void *user)
{
   pci_ingest_ctx_t *c = (pci_ingest_ctx_t *)user;
   if (!c)
      return;
   switch (event)
   {
   case SHELL_EVENT_TEXT_DELTA:
      if (data)
         dstr_append_str(&c->text, data);
      break;
   case SHELL_EVENT_SESSION_ID:
      /* A backend session id is short; ignore absurd values (bound the copy) and
       * only replace the last-known id on a SUCCESSFUL dup -- never drop a valid id
       * because a replacement alloc failed. */
      if (data && data[0] && strlen(data) < 128)
      {
         char *dup = strdup(data);
         if (dup)
         {
            free(c->session);
            c->session = dup;
         }
      }
      break;
   case SHELL_EVENT_TOOL_START:
      /* Native CLI tool use is observed post-hoc for audit; it is NOT gated here
       * (the shell-tool bypass is a separate track -- consult [21]). */
      c->tool_calls++;
      break;
   case SHELL_EVENT_ERROR:
      if (data && data[0] && !c->error[0])
         snprintf(c->error, sizeof c->error, "%s", data);
      break;
   case SHELL_EVENT_TOOL_COMPLETE:
   case SHELL_EVENT_TURN_COMPLETE:
      break;
   }
}

void primary_cli_turn_result_free(primary_cli_turn_result_t *r)
{
   if (!r)
      return;
   free(r->text);
   r->text = NULL;
   free(r->session);
   r->session = NULL;
}

int primary_cli_ingestor_turn(const char *session_id, const char *message, const char *repo,
                              const char *driver_name, const char *resume_id,
                              primary_cli_turn_result_t *out, volatile int *interrupted)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);

   /* Enforce BEFORE send: S1 route + S2 bind/guard are preventive for the turn
    * (consult [13]). A missing session id makes this a no-op (bound=0) but the
    * turn still runs -- unmanaged, never a silent pretend-enforced. */
   out->bound = primary_cli_ingestor_enforce_preturn(session_id, message, repo);

   const agent_shell_driver_t *driver =
       agent_shell_driver_get(driver_name && driver_name[0] ? driver_name : "claude");
   if (!driver || !driver->open || !driver->send || !driver->recv || !driver->close)
   {
      snprintf(out->error, sizeof out->error, "no CLI backend driver '%s'",
               driver_name && driver_name[0] ? driver_name : "claude");
      return -1;
   }

   void *handle = driver->open(driver, resume_id);
   if (!handle)
   {
      snprintf(out->error, sizeof out->error, "CLI backend open failed");
      return -1;
   }

   pci_ingest_ctx_t ctx;
   memset(&ctx, 0, sizeof ctx);
   dstr_init(&ctx.text);

   int rc = -1;
   if (driver->send(handle, message ? message : "") != 0)
   {
      snprintf(out->error, sizeof out->error, "CLI backend send failed");
      rc = -1;
   }
   else
   {
      rc = driver->recv(handle, pci_ingest_cb, &ctx, interrupted);
   }
   driver->close(handle);

   /* Honor the header contract: a -1 return always carries a message. Prefer the
    * driver's own error event; else a generic fallback (recv can fail without
    * emitting SHELL_EVENT_ERROR). */
   if (ctx.error[0] && !out->error[0])
      snprintf(out->error, sizeof out->error, "%s", ctx.error);
   if (rc != 0 && !out->error[0])
      snprintf(out->error, sizeof out->error, "CLI backend turn failed (rc=%d)", rc);
   out->text = (ctx.text.len && ctx.text.data) ? strdup(ctx.text.data) : NULL;
   out->session = ctx.session; /* transfer ownership (already strdup'd) */
   out->tool_calls = ctx.tool_calls;
   dstr_free(&ctx.text);
   return rc;
}
