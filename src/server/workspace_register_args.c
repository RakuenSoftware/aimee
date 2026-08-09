/* workspace_register_args.c: the flag arguments a REST workspace registration
 * hands to `workspace.add`.
 *
 * Its own translation unit for two reasons: server_http_routes.c, where the
 * route lives, is at its line ceiling; and this is pure string selection with no
 * dependencies, so a test can link it alone rather than dragging in the server. */
#include "server_http_internal.h"

/* Build the `workspace.add` flag arguments. `remote` and `head` are emitted only
 * when non-empty; a `mirror` registration REQUIRES both (the server seeds its
 * bare mirror by fetching that head from that remote), so dropping them turns a
 * mirror registration into a rejection. Borrows the caller's strings — the
 * pointers must outlive the returned array. Returns the count written. */
int workspace_add_flag_args(const char *provider, const char *remote, const char *head,
                            const char *out[], int out_cap)
{
   int n = 0;
   if (!out || out_cap < 2 || !provider || !provider[0])
      return 0;
   out[n++] = "--provider";
   out[n++] = provider;
   if (remote && remote[0] && n + 2 <= out_cap)
   {
      out[n++] = "--remote";
      out[n++] = remote;
   }
   if (head && head[0] && n + 2 <= out_cap)
   {
      out[n++] = "--head";
      out[n++] = head;
   }
   return n;
}
