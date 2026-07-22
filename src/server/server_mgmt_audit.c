#include "server_mgmt_audit.h"
#include <aimee/audit/audit_worm.h>
#include <stdio.h>
#include <string.h>

static int valid(const char *s)
{
   if (!s || strlen(s) > 256)
      return 0;
   for (; *s; s++)
      if ((unsigned char)*s < 0x20)
         return 0;
   return 1;
}
static int append(const char *action, const char *actor, const char *target, const char *cap,
                  const char *jti, const char *digest, const char *verdict)
{
   if (!valid(actor) || !valid(target) || !valid(cap) || !valid(jti) || !valid(digest))
      return -1;
   char detail[1400];
   snprintf(detail, sizeof(detail), "actor=%s target=%s capability=%s jti=%s request_digest=%s",
            actor, target, cap, jti, digest);
   return audit_worm_append("management", actor, action, target, verdict, detail);
}
int server_mgmt_audit_intent(const char *a, const char *t, const char *c, const char *j,
                             const char *d)
{
   return append("management.intent", a, t, c, j, d, "intent");
}
int server_mgmt_audit_outcome(const char *a, const char *t, const char *c, const char *j,
                              const char *d, int st)
{
   char v[32];
   snprintf(v, sizeof(v), "status_%d", st);
   return append("management.outcome", a, t, c, j, d, v);
}
int server_mgmt_dispatch_audited(const char *a, const char *t, const char *c, const char *j,
                                 const char *d, server_mgmt_action_fn fn, void *ctx)
{
   if (!fn || server_mgmt_audit_intent(a, t, c, j, d) != 0)
      return -1;
   int st = fn(ctx);
   if (server_mgmt_audit_outcome(a, t, c, j, d, st) != 0)
      return -1;
   return st;
}
