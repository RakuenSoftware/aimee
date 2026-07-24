#ifndef DEC_KB_WITNESS_GATE_STATE_H
#define DEC_KB_WITNESS_GATE_STATE_H 1

/* The one cross-thread cell in the witness release path: the result of the most
 * recent continuous verification pass. WRITTEN by the cadence (kb_main periodic
 * loop) thread and READ by the HTTP egress gate (listener thread, kb_http_egress.c)
 * — a genuine producer/consumer hand-off that gates real production egress.
 *
 * It lives in its own dependency-free translation unit for two reasons: the cell is
 * the entire concurrency contract (isolating it keeps that contract in one small,
 * auditable place), and it lets the race harness (tests/test_witness_gate_race.c,
 * run under ThreadSanitizer by scripts/run-witness-gate-tsan.sh) exercise the exact
 * store/load the gate relies on without linking the whole cadence/DB/vault stack.
 *
 * Values: -1 = never verified yet (fail-closed: the gate treats it as NOT clean);
 *          0 = last pass was not clean; 1 = last pass was clean. */

/* Publish the latest verification result. Atomic store with release ordering so a
 * clean->not-clean flip is visible to the very next gate read. */
void kb_witness_gate_state_set(int clean);

/* Read the latest verification result. Atomic load with acquire ordering. */
int kb_witness_gate_state_get(void);

#endif /* DEC_KB_WITNESS_GATE_STATE_H */
