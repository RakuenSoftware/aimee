#ifndef AIMEE_DELEGATE_LAUNCH_ARGS_H
#define AIMEE_DELEGATE_LAUNCH_ARGS_H 1

#include <aimee/delegates/module_api.h>
#include <stddef.h>
#include <stdint.h>

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

/* Stage 19: what a delegate plan becomes.
 *
 * This seam passes BYTES rather than a struct, unlike its neighbours. The
 * request carries a whole plan -- packets, owned files, per-file existence and
 * basename candidates -- and the response carries steps, coord tasks and the
 * briefs. Marshalling that through a C struct would mean inventing a second
 * representation of the plan and keeping it in step with the wire; the caller
 * already holds the cJSON and the filesystem facts, so it encodes directly with
 * the helpers in module_api.h.
 *
 * Fails closed: with no provider the launch is refused, which is the safe
 * direction -- nothing is written and the operator is told why. */
typedef int (*delegate_launch_plan_fn)(const uint8_t *request, size_t request_len,
                                       uint8_t *response, size_t response_cap,
                                       size_t *response_len);

void delegate_register_launch_plan_provider(delegate_launch_plan_fn provider);

/* Returns 0 and fills `response` on success, -1 on any failure (logged). */
int delegate_launch_plan_call(const uint8_t *request, size_t request_len, uint8_t *response,
                              size_t response_cap, size_t *response_len);

/* Stage 20: does this review show it looked at the code?
 *
 * `verdict` receives AIMEE_DELEGATES_REVIEW_* flags and `message` the
 * contradiction wording, if any.
 *
 * Fails CLOSED, and here that means NOT GUARDED: with no provider the review is
 * accepted rather than rejected. Rejecting on a missing provider would fail
 * honest reviews for a reason the operator cannot see or fix, which is worse
 * than losing a check that only ever catches a specific dishonesty. */
typedef int (*delegate_review_evidence_fn)(const char *role, const char *response, unsigned flags,
                                           unsigned *verdict, char *message, size_t message_cap);

void delegate_register_review_evidence_provider(delegate_review_evidence_fn provider);

/* Returns 0 with *verdict filled, or -1 on any failure (logged, verdict 0). */
int delegate_review_evidence_judge(const char *role, const char *response, unsigned flags,
                                   unsigned *verdict, char *message, size_t message_cap);

/* Stage 21: did the delegate touch the files its brief named?
 *
 * Bytes rather than a struct, for the same reason as the launch-plan seam: the
 * request carries a whole set of paths with their facts, and the caller already
 * holds them.
 *
 * Fails CLOSED, and here that means NO DRIFT: with no provider the delegate is
 * accepted. A hard verdict FAILS a delegate that may have done its job
 * perfectly, so inventing one because a module is missing would turn an
 * infrastructure problem into a wrong answer about the user's work. */
typedef int (*delegate_drift_fn)(const uint8_t *request, size_t request_len, unsigned *severity,
                                 char *message, size_t message_cap);

void delegate_register_drift_provider(delegate_drift_fn provider);

/* Returns 0 with *severity filled, or -1 on any failure (logged, severity 0). */
int delegate_drift_judge(const uint8_t *request, size_t request_len, unsigned *severity,
                         char *message, size_t message_cap);

/* Stage 15: what a delegate may do.
 *
 * Bytes rather than a struct, like the other seams that carry a set: the caller
 * holds the role and whatever definition an operator wrote, and the answer is a
 * list whose length nobody knows in advance.
 *
 * Fails CLOSED, which here means NO PERMISSIONS. A delegate whose powers cannot
 * be established gets none: it reads, and it changes nothing. That is the safe
 * direction for every permission in the set, which is not true of most seams in
 * this file and is why it is stated here. */
typedef int (*delegate_permissions_fn)(const uint8_t *request, size_t request_len,
                                       uint8_t *response, size_t response_cap,
                                       size_t *response_len);

void delegate_register_permissions_provider(delegate_permissions_fn provider);

/* Returns 0 and fills `response` on success, -1 on any failure (logged). */
int delegate_permissions_resolve(const uint8_t *request, size_t request_len, uint8_t *response,
                                 size_t response_cap, size_t *response_len);

/* The resolved set, held for the life of a delegate.
 *
 * Resolve ONCE when the delegate is created, then read it. Every query below is
 * a local lookup against what was resolved, not another question: a permission
 * asked twice is a permission that can answer differently the second time, which
 * is the defect this replaced. */
#define DELEGATE_PERM_MAX        16
#define DELEGATE_PERM_NAME_MAX   64
#define DELEGATE_PERM_SCOPE_MAX  8
#define DELEGATE_PERM_OBJECT_MAX 512
#define DELEGATE_PERM_TOOL_MAX   32

typedef struct
{
   char name[DELEGATE_PERM_NAME_MAX];
   char enforced_at[DELEGATE_PERM_NAME_MAX];
   char scopes[DELEGATE_PERM_SCOPE_MAX][DELEGATE_PERM_OBJECT_MAX];
   int scope_count;
} delegate_grant_t;

typedef struct
{
   delegate_grant_t grants[DELEGATE_PERM_MAX];
   int count;
   /* Permissions the module reported as held with nothing enforcing them. An
    * operator declared them and no point evaluates them, so they are surfaced
    * rather than treated as granted or as denied. */
   char unenforced[DELEGATE_PERM_MAX][DELEGATE_PERM_NAME_MAX];
   int unenforced_count;
   /* Tools this set withholds, whatever toolset the delegate resolved to. The
    * module decides which tool needs which permission; this side filters the
    * advertised array and refuses at dispatch against the same list, so what is
    * offered and what is allowed cannot disagree. */
   char denied_tools[DELEGATE_PERM_TOOL_MAX][DELEGATE_PERM_NAME_MAX];
   int denied_tool_count;
   int resolved;
} delegate_permissions_t;

/* Resolve what a role may do.
 *
 * `definition` is the role template's frontmatter when the operator wrote one,
 * or NULL for a role that ships as-is. Pass the TEXT, not a parse of it: the
 * module reads it, so there is one reading.
 *
 * Returns 0 on success. On failure `out` is zeroed, which holds NOTHING: a
 * delegate whose powers cannot be established reads and changes nothing. A
 * definition the module refuses fails HERE rather than quietly resolving to the
 * built-in set, which would hand the delegate the powers its role ships with
 * while the operator believes it holds the ones they wrote. */
int delegate_permissions_for_role(const char *role, const char *definition,
                                  delegate_permissions_t *out);

/* Whether the permission is held at all, ignoring any narrowing.
 *
 * Use where the object is not yet known, such as deciding whether to mount a
 * workspace read-write at all. Where the object IS known, use
 * delegate_permissions_allow, which this would answer yes to when the scope
 * forbids it. */
int delegate_permissions_has(const delegate_permissions_t *p, const char *permission);

/* Whether the permission covers this object. Unscoped grants cover everything;
 * scoped grants match exactly. */
int delegate_permissions_allow(const delegate_permissions_t *p, const char *permission,
                               const char *object);

/* Whether this set withholds a tool. Answered from the list the module sent, so
 * the filter that advertises tools and the dispatch that runs them agree. */
int delegate_permissions_denies_tool(const delegate_permissions_t *p, const char *tool);

#endif
