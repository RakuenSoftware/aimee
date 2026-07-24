#ifndef AIMEE_KB_WITNESS_CADENCE_H
#define AIMEE_KB_WITNESS_CADENCE_H

#include <time.h>

/* P7-witness-e2: the checkpoint cadence. Driven from the kb main periodic loop
 * (no separate thread), it calls db2_witness_checkpoint_produce() at most once per
 * interval. The producer holds a brief REPEATABLE READ transaction and is fenced,
 * so ticking from the main loop is safe. Failures are logged as integrity alerts
 * where they matter and skipped where they are benign (a fresh idle kb, a
 * transient serialization loss); a stalled checkpoint is a degradation, never a
 * crash and never an admission gate.
 */

#ifdef __cplusplus
extern "C"
{
#endif

/* Interval between checkpoint attempts, seconds. Kept modest so the latest signed
 * root does not age far behind the evidence, without churning on every tick. */
#define KB_WITNESS_CHECKPOINT_INTERVAL_S 60

/* Release-gate freshness bound: how stale the latest signed checkpoint may be
 * before the P2b egress gate closes. Set well above the checkpoint interval so a
 * few missed ticks (a transient serialization loss, a brief DB hiccup) do not close
 * egress, while a genuinely stalled chain — the state in which a coherent local
 * rewrite would no longer be caught by a fresh signed root — does. */
#define KB_WITNESS_CHECKPOINT_MAX_AGE_S 900

/* Call once per main-loop iteration with the current time. Produces a checkpoint
 * when the interval has elapsed; otherwise returns immediately. */
void kb_witness_cadence_tick(time_t now);

#include <stddef.h>

/* Boot fail-closed check. On a key-holding kb (kb_vault_live_keys_allowed) the
 * witness signer must be functional — its key derivable and its public anchor
 * available — before the kb serves, so no org key is ever used on a kb that cannot
 * sign the evidence of that use. Returns 0 when startup may proceed, -1 with a
 * reason in `err` when a key-holding kb must refuse to start. On a dev/no-live-key
 * kb it is a no-op (returns 0): there are no org keys to witness. */
int kb_witness_boot_check(char *err, size_t errlen);

/* The result of the most recent continuous checkpoint verification, for the P2b
 * release gate's "last verification was clean" term. Returns 1 only if the last
 * pass was clean; 0 if it was not; and a negative value if verification has not run
 * yet since boot. The gate treats anything other than 1 as fail-closed. */
int kb_witness_verification_last_clean(void);

#ifdef __cplusplus
}
#endif

#endif /* AIMEE_KB_WITNESS_CADENCE_H */
