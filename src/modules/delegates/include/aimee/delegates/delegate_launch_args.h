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

/* Which built sandbox images may be deleted.
 *
 * `request` is built with the imggc encoders above; `response` receives the
 * raw reply for the per-image readers. FAILS CLOSED: with no provider there is
 * no verdict and nothing is deleted, which is the safe direction -- an image
 * kept costs disk, an image deleted in error costs a rebuild of something that
 * may be in use. */
typedef int (*delegate_image_gc_fn)(const uint8_t *request, size_t request_len, uint8_t *response,
                                    size_t response_cap, size_t *response_len);

void delegate_register_image_gc_provider(delegate_image_gc_fn provider);

int delegate_image_gc_judge(const uint8_t *request, size_t request_len, uint8_t *response,
                            size_t response_cap, size_t *response_len);

/* Which agents in a fleet may serve this packet.
 *
 * FAILS CLOSED: with no provider there is no verdict and the caller must refuse
 * to route, rather than routing on requirements nothing checked -- the point of
 * the filter is that a packet needing tools or a large window does not land on
 * an agent that has neither. */
typedef int (*delegate_route_filter_fn)(const uint8_t *request, size_t request_len,
                                        uint8_t *response, size_t response_cap,
                                        size_t *response_len);

void delegate_register_route_filter_provider(delegate_route_filter_fn provider);

int delegate_route_filter_apply(const uint8_t *request, size_t request_len, uint8_t *response,
                                size_t response_cap, size_t *response_len);

/* Did a successful write delegate actually change anything?
 *
 * FAILS OPEN, unlike the other seams here, and deliberately: with no verdict
 * the run is ACCEPTED. This guard exists to catch a delegate that did nothing;
 * failing closed would reject completed work over a missing judgement, which is
 * the more damaging error of the two. */
typedef int (*delegate_noop_write_fn)(unsigned flags, int named_count, int *noop, int *benign,
                                      char *message, size_t message_cap);

void delegate_register_noop_write_provider(delegate_noop_write_fn provider);

/* Returns 1 when the run must be treated as incomplete, 0 otherwise. `message`
 * carries the wording for both the refusal and the benign notes. */
int delegate_noop_write_judge(unsigned flags, int named_count, int *benign, char *message,
                              size_t message_cap);

#endif
