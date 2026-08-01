#include "kb_witness_gate_state.h"

#include <stdatomic.h>

/* See kb_witness_gate_state.h. A C11 _Atomic, not a bare `volatile int`: volatile
 * forces the compiler to emit the access but the C memory model does not define it
 * for inter-thread synchronisation, so a bare volatile store could stay invisible to
 * the reader thread on a weakly-ordered target. Release/acquire ordering makes the
 * hand-off correct by construction. This gates production egress, so the ordering
 * matters. */
static _Atomic int g_last_verify_clean = -1;

void kb_witness_gate_state_set(int clean)
{
   atomic_store_explicit(&g_last_verify_clean, clean, memory_order_release);
}

int kb_witness_gate_state_get(void)
{
   return atomic_load_explicit(&g_last_verify_clean, memory_order_acquire);
}
