#ifndef DEC_CLI_ACP_H
#define DEC_CLI_ACP_H 1

#include <stddef.h>
#include "provider_cli_adapter.h"
#include <aimee/translation/harness_ir.h>

/* acp_turn_state_t: accumulator for one outbound ACP turn's streamed events. */
typedef struct
{
   char *text;     /* heap-accumulated reply text (caller frees)            */
   size_t len;     /* length of text                                        */
   int tool_calls; /* number of tool-call events observed this turn         */
   int done;       /* set once the terminal prompt response arrives         */
   int had_error;  /* set when an error response arrives                    */
   int prompt_id;  /* JSON-RPC id of the prompt request whose response ends  */
                   /* the turn; 0 means the legacy default of 2             */
   char error[256];
} acp_turn_state_t;

/* Consume one JSON-RPC line emitted by an outbound ACP agent into `st`.
 * Understands both aimee's legacy dialect (text/delta, tool/call) and standard
 * ACP session/update notifications (string or {type,text} content, and
 * tool_call updates), plus the terminal result(id==2)/error responses. A blank
 * or unparseable line is ignored. Returns 0 normally, -1 if the line was an
 * error response (st->had_error and st->error populated). */
int acp_turn_consume(const char *line, acp_turn_state_t *st);

/* Dispatch an already-captured native client request through its exact admitted
 * acp resource owner. No filesystem fallback. Permission decisions must come
 * from the current control owner or explicit host autonomous policy. */
int acp_dispatch_client_request(const cJSON *frame, const provider_cli_cfg_t *cfg,
                                aimee_harness_ir_t *ir, const cJSON *decision, cJSON **reply);

#endif /* DEC_CLI_ACP_H */
