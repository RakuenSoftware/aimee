/* delegate_launch_args.c: see delegate_launch_args.h */
#include <aimee/delegates/delegate_launch_args.h>
#include <aimee/delegates/module_api.h>

#include "log.h"

#include <stdio.h>
#include <string.h>

static delegate_launch_args_fn g_launch_args;

void delegate_register_launch_args_provider(delegate_launch_args_fn provider)
{
   g_launch_args = provider;
}

int delegate_launch_args_resolve(const aimee_delegates_launch_spec_t *spec, char *name_out,
                                 size_t name_cap, const char **argv_out, size_t argv_cap,
                                 uint8_t *buf, size_t buf_cap)
{
   if (!spec || !name_out || name_cap == 0 || !argv_out || argv_cap == 0 || !buf || buf_cap == 0)
      return -1;
   if (!g_launch_args)
   {
      /* No provider: nothing knows what this container should look like, and a
       * container assembled here would be a second copy of the rule with none
       * of the checks. Refuse instead. */
      LOG_ERROR("delegate-backend-docker",
                "no launch-args provider registered; refusing to create a delegate container "
                "whose shape nothing decided");
      return -1;
   }

   /* argv_cap-1 so the NULL terminator the caller needs always has a slot. */
   size_t lens[128];
   size_t max_args = argv_cap - 1;
   if (max_args > sizeof(lens) / sizeof(lens[0]))
      max_args = sizeof(lens) / sizeof(lens[0]);

   int argc = g_launch_args(spec, name_out, name_cap, argv_out, max_args, lens, buf, buf_cap);
   if (argc < 0)
      return -1;

   /* The decoded entries are length-prefixed, not NUL-terminated, and execve
    * needs NUL. Terminating in place overwrites the first byte of the NEXT
    * length field, so it is safe only once every length has been read -- which
    * the decode above has already done. The last entry writes one byte past the
    * response, which is why the provider is given a buffer with slack. */
   for (int i = 0; i < argc; i++)
      ((char *)argv_out[i])[lens[i]] = '\0';
   argv_out[argc] = NULL;
   return argc;
}

static delegate_image_spec_fn g_image_spec;

void delegate_register_image_spec_provider(delegate_image_spec_fn provider)
{
   g_image_spec = provider;
}

int delegate_image_spec_resolve(const char *base, const char *const *pkgs, int npkgs,
                                const char *verbatim, char *tag, size_t tag_cap, char *dockerfile,
                                size_t df_cap)
{
   if (!tag || !tag_cap || !dockerfile || !df_cap)
      return -1;
   tag[0] = '\0';
   dockerfile[0] = '\0';
   if (!g_image_spec)
   {
      LOG_ERROR("delegate-sandbox-image",
                "no image-spec provider registered; refusing to build a sandbox image whose "
                "contents nothing validated");
      return -1;
   }
   return g_image_spec(base, pkgs, npkgs, verbatim, tag, tag_cap, dockerfile, df_cap);
}

static delegate_isolation_fn g_isolation;

void delegate_register_isolation_provider(delegate_isolation_fn provider)
{
   g_isolation = provider;
}

int delegate_isolation_judge(const char *report, int probe_failed, int require_isolation,
                             int *refuse, int *warn, int *is_error, char *reason, size_t reason_cap)
{
   if (!refuse || !warn || !is_error)
      return -1;
   *refuse = 0;
   *warn = 0;
   *is_error = 0;
   if (reason && reason_cap)
      reason[0] = '\0';
   if (!g_isolation)
   {
      LOG_ERROR("delegate-sandbox",
                "no isolation provider registered; the sandbox cannot be judged isolated");
      return -1;
   }
   return g_isolation(report, probe_failed, require_isolation, refuse, warn, is_error, reason,
                      reason_cap);
}

static delegate_image_gc_fn g_image_gc;

void delegate_register_image_gc_provider(delegate_image_gc_fn provider)
{
   g_image_gc = provider;
}

int delegate_image_gc_judge(const uint8_t *request, size_t request_len, uint8_t *response,
                            size_t response_cap, size_t *response_len)
{
   if (!request || !response || !response_len)
      return -1;
   *response_len = 0;
   if (!g_image_gc)
   {
      LOG_ERROR("delegate-sandbox-image",
                "no image-gc provider registered; keeping every image rather than deleting on "
                "a policy nothing applied");
      return -1;
   }
   return g_image_gc(request, request_len, response, response_cap, response_len);
}

static delegate_route_filter_fn g_route_filter;

void delegate_register_route_filter_provider(delegate_route_filter_fn provider)
{
   g_route_filter = provider;
}

int delegate_route_filter_apply(const uint8_t *request, size_t request_len, uint8_t *response,
                                size_t response_cap, size_t *response_len)
{
   if (!request || !response || !response_len)
      return -1;
   *response_len = 0;
   if (!g_route_filter)
   {
      LOG_ERROR("delegate.route",
                "no route-filter provider registered; refusing to route on requirements nothing "
                "checked");
      return -1;
   }
   return g_route_filter(request, request_len, response, response_cap, response_len);
}

static delegate_noop_write_fn g_noop_write;
static delegate_launch_plan_fn g_launch_plan;
static delegate_review_evidence_fn g_review_evidence;
static delegate_drift_fn g_drift;
static delegate_permissions_fn g_permissions;

void delegate_register_noop_write_provider(delegate_noop_write_fn provider)
{
   g_noop_write = provider;
}

int delegate_noop_write_judge(unsigned flags, int named_count, int *benign, char *message,
                              size_t message_cap)
{
   if (benign)
      *benign = 0;
   if (message && message_cap)
      message[0] = '\0';
   if (!g_noop_write)
   {
      /* Fails OPEN. This guard catches a delegate that did nothing; rejecting
       * completed work because the guard could not run is the worse error. */
      LOG_WARN("delegate", "no no-op-write provider registered; accepting the run unjudged");
      return 0;
   }
   int noop = 0, b = 0;
   if (g_noop_write(flags, named_count, &noop, &b, message, message_cap) != 0)
   {
      LOG_WARN("delegate", "no-op-write check could not be evaluated; accepting the run");
      return 0;
   }
   if (benign)
      *benign = b;
   return noop;
}

void delegate_register_launch_plan_provider(delegate_launch_plan_fn provider)
{
   g_launch_plan = provider;
}

int delegate_launch_plan_call(const uint8_t *request, size_t request_len, uint8_t *response,
                              size_t response_cap, size_t *response_len)
{
   if (!g_launch_plan)
   {
      LOG_ERROR("delegates", "no launch-plan provider registered; refusing to launch");
      return -1;
   }
   if (!request || !response || !response_len)
      return -1;
   return g_launch_plan(request, request_len, response, response_cap, response_len);
}

void delegate_register_review_evidence_provider(delegate_review_evidence_fn provider)
{
   g_review_evidence = provider;
}

int delegate_review_evidence_judge(const char *role, const char *response, unsigned flags,
                                   unsigned *verdict, char *message, size_t message_cap)
{
   if (verdict)
      *verdict = 0;
   if (message && message_cap)
      message[0] = '\0';
   if (!g_review_evidence)
   {
      LOG_ERROR("delegates",
                "no review-evidence provider registered; accepting the review unchecked");
      return -1;
   }
   return g_review_evidence(role, response, flags, verdict, message, message_cap);
}

void delegate_register_drift_provider(delegate_drift_fn provider)
{
   g_drift = provider;
}

int delegate_drift_judge(const uint8_t *request, size_t request_len, unsigned *severity,
                         char *message, size_t message_cap)
{
   if (severity)
      *severity = 0;
   if (message && message_cap)
      message[0] = '\0';
   if (!g_drift)
   {
      LOG_ERROR("delegates", "no drift provider registered; accepting the delegate unchecked");
      return -1;
   }
   return g_drift(request, request_len, severity, message, message_cap);
}

void delegate_register_permissions_provider(delegate_permissions_fn provider)
{
   g_permissions = provider;
}

int delegate_permissions_resolve(const uint8_t *request, size_t request_len, uint8_t *response,
                                 size_t response_cap, size_t *response_len)
{
   if (response_len)
      *response_len = 0;
   if (!g_permissions)
   {
      LOG_ERROR("delegates",
                "no permissions provider registered; the delegate holds nothing and may only read");
      return -1;
   }
   if (!request || !response || !response_len)
      return -1;
   return g_permissions(request, request_len, response, response_cap, response_len);
}

/* Room for a role name and the answer. Both are small; the cap exists so a
 * malformed response cannot be believed rather than to bound real data. */
#define DELEGATE_PERM_WIRE_CAP 8192u

/* Larger than every destination in delegate_permissions_t, so a value that
 * overflows a field is visible here rather than silently shortened. */
#define PERMS_SCRATCH 1024

/* Copy an answer into the held set, or fail.
 *
 * The wire reader truncates a string that does not fit and says nothing, which
 * for a PERMISSION name is a different permission: `Has("deploy_to_production")`
 * against a name cut at 63 characters simply does not match, and the delegate
 * quietly holds less (or, for a withheld tool, quietly holds MORE) than the
 * operator wrote. The scratch buffer is larger than every destination, so
 * anything that does not fit here did not fit the wire read either. */
static int perms_copy(char *dst, size_t dst_cap, const char *src, const char *what)
{
   size_t len = strlen(src);
   if (len >= dst_cap)
   {
      LOG_ERROR("delegates", "%s '%.32s...' is longer than this build can hold (%zu >= %zu)", what,
                src, len, dst_cap);
      return -1;
   }
   memcpy(dst, src, len + 1);
   return 0;
}

/* A count the held set cannot store. Refusing is the only honest answer: a
 * silently shortened list of GRANTS is a delegate with powers the operator did
 * not write, and a silently shortened list of DENIED TOOLS is a delegate holding
 * tools they withheld. */
static int perms_fits(int count, int limit, const char *what)
{
   if (count > limit)
   {
      LOG_ERROR("delegates", "%s: %d is more than this build can hold (%d)", what, count, limit);
      return 0;
   }
   return 1;
}

int delegate_permissions_for_role(const char *role, const char *definition,
                                  delegate_permissions_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof(*out));

   uint8_t request[DELEGATE_PERM_WIRE_CAP];
   aimee_delegates_wire_t w;
   unsigned flags = (definition && definition[0]) ? AIMEE_DELEGATES_PERMS_DEFINED : 0u;
   aimee_delegates_perms_request_begin(&w, request, sizeof(request), flags, role ? role : "");
   if (flags)
      aimee_delegates_perms_request_definition(&w, definition);
   if (w.overflow)
      return -1;

   uint8_t response[DELEGATE_PERM_WIRE_CAP];
   size_t response_len = 0;
   if (delegate_permissions_resolve(request, w.len, response, sizeof(response), &response_len) != 0)
      return -1;

   aimee_delegates_rd_t r;
   int count = aimee_delegates_perms_response_begin(&r, response, response_len);
   if (count < 0)
      return -1;

   if (!perms_fits(count, DELEGATE_PERM_MAX, "permissions held"))
      goto too_big;

   for (int i = 0; i < count; i++)
   {
      char name[PERMS_SCRATCH], enforced_at[PERMS_SCRATCH];
      int scope_count = 0;
      if (aimee_delegates_perms_response_grant(&r, name, sizeof(name), enforced_at,
                                               sizeof(enforced_at), &scope_count) != 0)
         return -1;

      if (out->count >= DELEGATE_PERM_MAX)
         goto too_big; /* belt: the count was checked above, and this array is fixed */
      delegate_grant_t *grant = &out->grants[out->count++];
      if (perms_copy(grant->name, sizeof(grant->name), name, "permission") != 0 ||
          perms_copy(grant->enforced_at, sizeof(grant->enforced_at), enforced_at,
                     "enforcement point") != 0)
         goto too_big;

      if (!perms_fits(scope_count, DELEGATE_PERM_SCOPE_MAX, "scopes on one permission"))
         goto too_big;
      for (int scope = 0; scope < scope_count; scope++)
      {
         char object[PERMS_SCRATCH];
         aimee_delegates_rd_str(&r, object, sizeof(object));
         if (grant->scope_count >= DELEGATE_PERM_SCOPE_MAX)
            goto too_big; /* belt */
         if (perms_copy(grant->scopes[grant->scope_count++], DELEGATE_PERM_OBJECT_MAX, object,
                        "scope") != 0)
            goto too_big;
      }
   }

   uint32_t unenforced = aimee_delegates_rd_u32(&r);
   if (!r.bad && !perms_fits((int)unenforced, DELEGATE_PERM_MAX, "unenforced permissions"))
      goto too_big;
   for (uint32_t i = 0; i < unenforced && !r.bad; i++)
   {
      char name[PERMS_SCRATCH];
      aimee_delegates_rd_str(&r, name, sizeof(name));
      if (out->unenforced_count >= DELEGATE_PERM_MAX)
         goto too_big; /* belt */
      if (perms_copy(out->unenforced[out->unenforced_count++], DELEGATE_PERM_NAME_MAX, name,
                     "unenforced permission") != 0)
         goto too_big;
   }

   uint32_t denied = aimee_delegates_rd_u32(&r);
   if (!r.bad && !perms_fits((int)denied, DELEGATE_PERM_TOOL_MAX, "withheld tools"))
      goto too_big;
   for (uint32_t i = 0; i < denied && !r.bad; i++)
   {
      char name[PERMS_SCRATCH];
      aimee_delegates_rd_str(&r, name, sizeof(name));
      if (out->denied_tool_count >= DELEGATE_PERM_TOOL_MAX)
         goto too_big; /* belt */
      if (perms_copy(out->denied_tools[out->denied_tool_count++], DELEGATE_PERM_NAME_MAX, name,
                     "withheld tool") != 0)
         goto too_big;
   }

   if (r.bad)
   {
      memset(out, 0, sizeof(*out));
      return -1;
   }

   for (int i = 0; i < out->unenforced_count; i++)
      LOG_WARN("delegates",
               "role '%s' declares permission '%s' and nothing enforces it; it grants and denies "
               "nothing at runtime",
               role ? role : "", out->unenforced[i]);

   out->resolved = 1;
   return 0;

too_big:
   /* The answer did not fit, so we do not have the answer. Callers treat that as
      a refusal, which is the same thing they do when the module cannot be
      reached: a delegate whose powers cannot be established does not run. */
   memset(out, 0, sizeof(*out));
   return -1;
}

static const delegate_grant_t *delegate_permissions_find(const delegate_permissions_t *p,
                                                         const char *permission)
{
   if (!p || !permission)
      return NULL;
   for (int i = 0; i < p->count; i++)
      if (strcmp(p->grants[i].name, permission) == 0)
         return &p->grants[i];
   return NULL;
}

int delegate_permissions_has(const delegate_permissions_t *p, const char *permission)
{
   return delegate_permissions_find(p, permission) != NULL;
}

int delegate_permissions_allow(const delegate_permissions_t *p, const char *permission,
                               const char *object)
{
   const delegate_grant_t *grant = delegate_permissions_find(p, permission);
   if (!grant)
      return 0;
   if (grant->scope_count == 0)
      return 1;
   if (!object)
      return 0;
   for (int i = 0; i < grant->scope_count; i++)
      if (strcmp(grant->scopes[i], object) == 0)
         return 1;
   return 0;
}

int delegate_permissions_denies_tool(const delegate_permissions_t *p, const char *tool)
{
   if (!p || !tool)
      return 0;
   for (int i = 0; i < p->denied_tool_count; i++)
      if (strcmp(p->denied_tools[i], tool) == 0)
         return 1;
   return 0;
}
