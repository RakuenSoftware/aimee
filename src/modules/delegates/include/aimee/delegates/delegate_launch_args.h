#ifndef AIMEE_DELEGATE_LAUNCH_ARGS_H
#define AIMEE_DELEGATE_LAUNCH_ARGS_H 1

#include <aimee/delegates/module_api.h>
#include <stddef.h>

/* Where a delegate container's SHAPE comes from.
 *
 * The module decides the mounts, the environment, the container's name and the
 * create argv, together, and this is the seam the backend asks through. C does
 * not assemble an argv of its own: the guarantees the sandbox exists for -- no
 * network, no runtime socket, a read-only delegate that cannot receive a
 * writable workspace mount, no credential in the environment -- stop being
 * checkable the moment they are argv.
 *
 * FAILS CLOSED. With no provider registered there is no answer, and a delegate
 * whose shape is unknown does not run. That is the whole point of asking. */

/* Fills `name_out` and `argv_out` with pointers into `buf`, which the caller
 * supplies and must keep alive for as long as it uses the argv. Returns the
 * argument count, or -1. */
typedef int (*delegate_launch_args_fn)(const aimee_delegates_launch_spec_t *spec, char *name_out,
                                       size_t name_cap, const char **argv_out, size_t argv_cap,
                                       size_t *arg_len_out, uint8_t *buf, size_t buf_cap);

void delegate_register_launch_args_provider(delegate_launch_args_fn provider);

/* Ask for the create argv. Returns the argument count, or -1 (logged).
 *
 * The returned argv entries point into `buf`; they are NUL-terminated in place,
 * which is why `buf` is the caller's and not the seam's. */
int delegate_launch_args_resolve(const aimee_delegates_launch_spec_t *spec, char *name_out,
                                 size_t name_cap, const char **argv_out, size_t argv_cap,
                                 uint8_t *buf, size_t buf_cap);

/* The Dockerfile a sandbox image is built from, and the tag naming its content.
 *
 * Same seam, same reason: the package and base names are interpolated into a
 * file that `docker build` then executes WITH a network, so the rule that
 * validates them and the rule that renders them must be the same rule.
 *
 * FAILS CLOSED: with no provider there is no Dockerfile, and nothing is built. */
typedef int (*delegate_image_spec_fn)(const char *base, const char *const *pkgs, int npkgs,
                                      const char *verbatim, char *tag, size_t tag_cap,
                                      char *dockerfile, size_t df_cap);

void delegate_register_image_spec_provider(delegate_image_spec_fn provider);

/* Returns 0 and fills both, or -1 (logged). */
int delegate_image_spec_resolve(const char *base, const char *const *pkgs, int npkgs,
                                const char *verbatim, char *tag, size_t tag_cap, char *dockerfile,
                                size_t df_cap);

/* What a container's network report means, and what to do about it.
 *
 * FAILS CLOSED: with no provider there is no verdict, and a delegate whose
 * isolation nothing judged is refused. That is the same reasoning the judgement
 * itself uses -- an unproven sandbox is not a sandbox. */
typedef int (*delegate_isolation_fn)(const char *report, int probe_failed, int require_isolation,
                                     int *refuse, int *warn, int *is_error, char *reason,
                                     size_t reason_cap);

void delegate_register_isolation_provider(delegate_isolation_fn provider);

/* Returns 0 with the verdict filled, or -1 (logged) -- which callers must treat
 * as a refusal. */
int delegate_isolation_judge(const char *report, int probe_failed, int require_isolation,
                             int *refuse, int *warn, int *is_error, char *reason,
                             size_t reason_cap);

/* May this delegate write? The role and the brief, composed by the module.
 *
 * FAILS CLOSED: with no provider the answer is NO. A delegate that cannot be
 * shown to be permitted does not get a writable tree -- the mount is the
 * enforcement, so guessing yes is the one direction with no recovery. */
typedef int (*delegate_may_write_fn)(const char *role, const char *prompt, int *may_write,
                                     int *by_role, int *by_prompt);

void delegate_register_may_write_provider(delegate_may_write_fn provider);

/* Returns 1 when the delegate may write, 0 otherwise (including on any
 * failure, which is logged). */
int delegate_may_write(const char *role, const char *prompt);

#endif
