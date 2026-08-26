/* memory_activation.h: per-unit retrieval hysteresis.
 *
 * Recall is otherwise computed from scratch every turn, so a unit is either
 * above threshold or absent with nothing in between: it repeats every turn
 * while the topic holds, then vanishes the moment the phrasing drifts. This
 * gives each unit an activation state of its own, so the condition for staying
 * in is not the condition for getting in.
 *
 * Four independent parameters, evaluated as an ordered gate AFTER relevance has
 * already selected the candidate:
 *
 *   relevance -> delay -> cooldown -> suppression -> inject
 *
 *   sticky (N turns)   having fired, stays eligible for N turns regardless of
 *                      match, so a rephrase does not drop the thread.
 *   cooldown (M turns) having fired, refuses to fire again for M turns. This is
 *                      the only thing here that prevents per-turn repetition.
 *   delay (N turns)    will not fire until the conversation is N turns old,
 *                      keeping background material from leading.
 *   suppression        withheld regardless of match.
 *
 * The precedence between sticky and cooldown is stated rather than left
 * emergent, because leaving it implicit is the commonest defect in
 * implementations of this pattern: COOLDOWN WINS. Sticky answers "may this stay
 * eligible without matching", cooldown answers "may this fire at all"; a unit
 * inside its cooldown does not fire even while sticky, or cooldown would never
 * bind on exactly the units that keep matching.
 *
 * Two constraints are load-bearing and are the reason this reads its state from
 * the session store rather than a process-local map:
 *
 *   - Activation persists WITH THE CONVERSATION. State held only in a process
 *     silently resets its cooldowns on reload, and the repetition returns with
 *     no visible cause -- a failure that looks exactly like the feature working.
 *   - Injection MUST NOT feed the usage signal. What the harness surfaces is not
 *     evidence that the surfaced thing was useful. Activation is recorded on its
 *     own turn axis and never touches effectiveness, utility, or any other
 *     reinforcement path; otherwise this feature becomes a second place where
 *     exposure is mistaken for validation.
 */
#ifndef DEC_MEMORY_ACTIVATION_H
#define DEC_MEMORY_ACTIVATION_H 1

#include <stdint.h>

/* How many rows of one conversation's activation state to read per turn. */
#define MEMORY_ACTIVATION_MAX_ROWS 256

/* Ranking nudge applied to a sticky unit. Deliberately of the same order as the
 * scope and tier nudges beside it (0.05 and 0.03): enough to keep a unit in
 * play through a rephrase, not enough to float an irrelevant one to the top.
 * It moves REACHABILITY only -- confidence, effectiveness and every other
 * reinforcement signal are untouched, or being surfaced would start to count as
 * evidence of being right. */
#define MEMORY_ACTIVATION_STICKY_BONUS 0.04

typedef struct
{
   int64_t memory_id;
   int64_t last_turn;
} memory_activation_row_t;

typedef struct memory_activation
{
   memory_activation_row_t rows[MEMORY_ACTIVATION_MAX_ROWS];
   int count;
   int64_t current_turn; /* the turn being assembled; 0 when unavailable */
   int loaded;           /* 0 when the store could not be read: gate stays open */
} memory_activation_t;

/* Read one conversation's activation state. On any failure -- no session, no
 * store, a wire error -- `out` is left not-loaded, and every predicate below
 * then answers "no opinion". Failing open is deliberate: a gate that errs
 * toward withholding evidence produces confident answers with nothing behind
 * them, which is worse than repeating a memory. */
void memory_activation_load(memory_activation_t *out, const char *session_id);

/* Copy the state most recently loaded by this thread for `session_id` without
 * advancing its persisted turn. Explain mode uses this after the production
 * assembly so diagnostics cannot create a phantom conversation turn. Returns
 * 1 on a matching snapshot, 0 otherwise. */
int memory_activation_last_loaded(const char *session_id, memory_activation_t *out);

/* The last turn `memory_id` fired on in this conversation, or 0 for never. */
int64_t memory_activation_last_turn(const memory_activation_t *act, int64_t memory_id);

/* 1 when `memory_id` is inside its cooldown and must not fire this turn. */
int memory_activation_in_cooldown(const memory_activation_t *act, int64_t memory_id,
                                  int cooldown_turns);

/* 1 when `memory_id` fired recently enough to stay eligible without matching
 * this turn. Callers must still apply the cooldown check: cooldown wins. */
int memory_activation_is_sticky(const memory_activation_t *act, int64_t memory_id,
                                int sticky_turns);

/* 1 until the persisted conversation turn is greater than delay_turns. A store
 * read failure returns 0 so missing activation state never withholds evidence. */
int memory_activation_is_delayed(const memory_activation_t *act, int delay_turns);

/* Record that `memory_id` was injected on this turn. Writes only the activation
 * axis; it deliberately does not touch any reinforcement or effectiveness
 * signal. */
void memory_activation_record(const memory_activation_t *act, const char *session_id,
                              int64_t memory_id, double score);

#endif /* DEC_MEMORY_ACTIVATION_H */
