/* session_state.c: load/save session_state_t via DB1. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "aimee.h"
#include "db1_client/db1.h"
#include <stdio.h>
#include <string.h>

/* Reset `state` to an empty struct with sane default modes. */
static void session_state_init_defaults(session_state_t *state)
{
   memset(state, 0, sizeof(*state));
   snprintf(state->session_mode, sizeof(state->session_mode), "%s", MODE_IMPLEMENT);
   snprintf(state->guardrail_mode, sizeof(state->guardrail_mode), "%s", MODE_APPROVE);
   snprintf(state->tdd_mode, sizeof(state->tdd_mode), "off");
}

void session_state_load(session_state_t *state, const char *sid)
{
   session_state_init_defaults(state);
   if (!sid || !sid[0])
      return;

   (void)db1_session_state_load(sid, state);
}

void session_state_save(const session_state_t *state, const char *sid)
{
   if (!state->dirty)
      return;
   if (!sid || !sid[0])
      return;
   db1_session_state_save(sid, state);
}

void session_state_force_save(const session_state_t *state, const char *sid)
{
   if (!sid || !sid[0])
      return;
   db1_session_state_save(sid, state);
}
