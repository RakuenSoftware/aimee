/* delegate_launch_args.c: see delegate_launch_args.h */
#include <aimee/delegates/delegate_launch_args.h>

#include "log.h"

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

static delegate_may_write_fn g_may_write;

void delegate_register_may_write_provider(delegate_may_write_fn provider)
{
   g_may_write = provider;
}

int delegate_may_write(const char *role, const char *prompt)
{
   if (!g_may_write)
   {
      LOG_ERROR("delegates",
                "no may-write provider registered; treating the delegate as read-only");
      return 0;
   }
   int may = 0, by_role = 0, by_prompt = 0;
   if (g_may_write(role, prompt, &may, &by_role, &by_prompt) != 0)
   {
      LOG_ERROR("delegates",
                "could not resolve write permission for role '%s'; "
                "treating the delegate as read-only",
                role ? role : "");
      return 0;
   }
   if (!may)
      LOG_INFO("delegates",
               "delegate role '%s' is read-only for this turn (role permits=%d, "
               "brief asks=%d)",
               role ? role : "", by_role, by_prompt);
   return may;
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
