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
