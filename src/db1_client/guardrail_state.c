/* db1_client/guardrail_state.c: the guardrail_state family, reached over the bus.
 *
 * WAS GENERATED from the store catalog by scripts/gen_db1_contract.py. Both
 * moved on: the catalog is now server-go/modules/aimee/operations.json, and the
 * generator was deleted with the C module.
 *
 * So this is maintained BY HAND now, and the header used to say "Do not edit"
 * while pointing at a generator that no longer exists and a path that no longer
 * resolves -- which is a dead end at exactly the moment someone needs to change
 * something. Edit it, and keep it agreeing with the catalog:
 * scripts/check-db1-client-contract.py matches every call site here against the
 * catalog by arity and reply width, and runs in lint on every pull request.
 * That check is what replaced the generator.
 *
 * Same functions, same contract, different side of the boundary: the daemon
 * links this instead of the DB1 domain, so nothing that calls these had to
 * change.
 *
 * It lives OUTSIDE modules/db1 deliberately. The module's descriptor owns every
 * .c beside it and compiles them into the DB1 process, so a client with these
 * names in that directory would be linked twice into the one binary that must
 * not have it -- once as the caller and once as the implementation.
 *
 * clang-format is off for the body below: its canonical form is whatever this
 * generator emits, and reflowing generated output would put the file and the
 * catalog permanently one reformat apart. */
/* clang-format off */
#include "session_state.h"

#include "db1_module_api.h"

#include <aimee/audit/obs_bus.h>
#include <aimee/core/event_bus/module_client.h>
#include <aimee/core/event_bus/module_protocol.h>
#include "log.h"
#include "module_json_call.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB1_GUARDRAIL_STATE_CALL_TIMEOUT_MS 2000

static void warn_unreachable(int reason)
{
   static int warned;
   if (warned)
      return;
   warned = 1;
   /* Said once per process: enough to tell a store that is down from one that
      is quiet, without one line per call. The numeric
      aimee_module_call_result_t, not its name, so this does not pull the whole
      event-bus library in behind the client for one string. */
   LOG_WARN("db1.guardrail_state", "DB1 %s is unreachable (module call result %d)", "guardrail state",
            reason);
}

/* Size the frame from the arguments themselves.

   These carry prompts, results and JSON documents, not just identifiers, and
   in-process callers have always passed them whole. A fixed cap here would
   refuse exactly those calls and return the same -1 as a broken store -- fine
   in a test with short strings, wrong the first time a real prompt arrives. The
   bus bounds the message instead. */
static int frame_size(const char *const *fields, uint32_t count, size_t *need_out)
{
   /* Zero fields is a legal request: an operation that takes no arguments
      sends the header alone. The upper bound still applies. */
   if (count > AIMEE_DB1_FIELDS_MAX)
      return -1;
   size_t need = 8u;
   for (uint32_t i = 0; i < count; ++i)
   {
      /* Empty is legal on the wire: an optional field the caller left out
         travels as zero length. Which fields may be empty is the operation's
         business, checked before the frame is built. */
      if (!fields[i])
         return -1;
      size_t n = strlen(fields[i]);
      if (n > AIMEE_MODULE_MESSAGE_MAX_BODY - need - 4u)
         return -1;
      need += 4u + n;
   }
   *need_out = need;
   return 0;
}

/* op(u32) | field_count(u32) | (len(u32) | bytes) * count, per db1_module_api.h. */
static void encode(uint8_t *out, uint32_t op, const char *const *fields, uint32_t count)
{
   uint32_t at = 0;
   aimee_db1_put_u32(out + at, op);
   at += 4u;
   aimee_db1_put_u32(out + at, count);
   at += 4u;
   for (uint32_t i = 0; i < count; ++i)
   {
      uint32_t n = (uint32_t)strlen(fields[i]);
      aimee_db1_put_u32(out + at, n);
      at += 4u;
      memcpy(out + at, fields[i], n);
      at += n;
   }
}

/* Returns the module's status, or -1 when the call never produced one. */
/* Fills up to `slots` reply values, each into the buffer and capacity the
   caller supplied. A write passes none; a read passes one; a row passes one per
   member; a list passes one per member per row it is willing to accept.

   `filled_out` reports how many values the reply actually carried, which is how
   a list learns its length: the rows are not counted separately on the wire
   because an operation already knows how wide its rows are. Callers that expect
   a fixed shape pass NULL. */
static int call_stage(uint32_t op, const char *const *fields, uint32_t count, char *const *values,
                      const size_t *caps, uint32_t slots, uint32_t *filled_out)
{
   if (filled_out)
      *filled_out = 0u;
   for (uint32_t i = 0; i < slots; ++i)
      if (values[i] && caps[i])
         values[i][0] = '\0';
   /* A local check, not a probe: with nothing serving the stage there is no
      call to make, and saying so beats waiting out a deadline. */
   if (!obs_bus_module_available(AIMEE_DB1_EVENT_GUARDRAIL_STATE))
   {
      warn_unreachable(AIMEE_MODULE_CALL_CAPABILITY_ABSENT);
      return -1;
   }

   size_t request_len = 0;
   if (frame_size(fields, count, &request_len) != 0)
      return -1;
   /* The reply is bounded by the caller's own buffer: it asked for at most
      value_len bytes, so there is no reason to hold more than that. */
   size_t response_cap = 8u;
   for (uint32_t i = 0; i < slots; ++i)
      response_cap += 4u + caps[i];
   uint8_t *request = malloc(request_len);
   uint8_t *response = malloc(response_cap);
   if (!request || !response)
   {
      free(request);
      free(response);
      return -1;
   }
   encode(request, op, fields, count);

   uint32_t response_len = 0;
   uint64_t deadline = aimee_module_call_deadline_ns(DB1_GUARDRAIL_STATE_CALL_TIMEOUT_MS);
   aimee_module_call_result_t rc =
       obs_bus_module_call(AIMEE_DB1_EVENT_GUARDRAIL_STATE, AIMEE_DB1_STAGE_GUARDRAIL_STATE, 0, deadline,
                           request, (uint32_t)request_len, response, (uint32_t)response_cap,
                           &response_len, NULL, NULL);
   free(request);

   int result = -1;
   if (rc != AIMEE_MODULE_CALL_OK || response_len < 8u)
      warn_unreachable((int)rc);
   else
   {
      uint32_t status = aimee_db1_get_u32(response);
      uint32_t fields_in = aimee_db1_get_u32(response + 4u);
      /* Read the reply's own count rather than assuming an arity: a status with
         no values is how a write answers, one value is a read, and a member
         apiece is a row. */
      result = (int)status;
      /* More values than the caller has room for is a contract mismatch, not
         something to read the first few of: the caller asked for at most this
         many rows, and a stage answering with more is not answering this call. */
      if (fields_in > slots)
         result = -1;
      else if (filled_out)
         *filled_out = fields_in;
      /* Fewer values than the caller has slots for is the same contract
         mismatch read from the other side, and it used to pass: the unfilled
         slots keep the empty string cleared above, so the caller reads a row
         whose last members are blank and cannot tell that from a row that is
         blank. A list says how many rows it found through filled_out and is
         variable by construction; every other shape has one arity, and a stage
         answering with a different one is a stage built against a different
         version of this contract. Two processes, two binaries, two deployment
         times -- so say it rather than zero-fill. */
      else if (status == (uint32_t)AIMEE_DB1_STATUS_OK && fields_in != slots)
         result = -1;
      uint32_t at = 8u;
      for (uint32_t i = 0; i < fields_in && result != -1; ++i)
      {
         if (at + 4u > response_len)
         {
            result = -1;
            break;
         }
         uint32_t n = aimee_db1_get_u32(response + at);
         at += 4u;
         /* A reply whose declared length runs past what arrived is not a reply
            to read part of. */
         if (at + n > response_len)
         {
            result = -1;
            break;
         }
         if (i < slots && values[i] && caps[i])
         {
            /* No room for the terminator is no room: writing it would land one
               byte past the buffer the caller owns. */
            if (n >= caps[i])
               result = -1;
            else
            {
               memcpy(values[i], response + at, n);
               values[i][n] = '\0';
            }
         }
         at += n;
      }
   }
   free(response);
   return result;
}

/* A write answers 0 or -1; the store either took it or it did not. */
static int write_result(int status)
{
   return status == (int)AIMEE_DB1_STATUS_OK ? 0 : -1;
}


int db1_session_state_load(const char *sid, session_state_t *out)
{
   if (!sid || !sid[0] || !out)
      return -1;
   const char *fields[] = {sid};
   char slot64[32];
   char slot67[32];
   char slot68[32];
   char slot69[32];
   char slot102[32];
   char slot103[32];
   char slot104[32];
   char slot105[32];
   char slot106[32];
   char slot107[32];
   char slot108[32];
   char slot111[32];
   char slot113[32];
   char slot115[32];
   char slot117[32];
   char slot119[32];
   char slot121[32];
   char slot123[32];
   char slot125[32];
   char slot126[32];
   char slot191[32];
   char slot193[32];
   char slot195[32];
   char slot197[32];
   char slot199[32];
   char slot201[32];
   char slot203[32];
   char slot205[32];
   char slot207[32];
   char slot209[32];
   char slot211[32];
   char slot213[32];
   char slot215[32];
   char slot217[32];
   char slot219[32];
   char slot221[32];
   char slot223[32];
   char slot225[32];
   char slot227[32];
   char slot229[32];
   char slot231[32];
   char slot233[32];
   char slot235[32];
   char slot237[32];
   char slot239[32];
   char slot241[32];
   char slot243[32];
   char slot245[32];
   char slot247[32];
   char slot249[32];
   char slot251[32];
   char slot253[32];
   char slot255[32];
   char slot257[32];
   char slot259[32];
   char slot261[32];
   char slot263[32];
   char slot265[32];
   char slot267[32];
   char slot269[32];
   char slot271[32];
   char slot273[32];
   char slot275[32];
   char slot277[32];
   char slot279[32];
   char slot281[32];
   char slot283[32];
   char slot285[32];
   char slot287[32];
   char slot289[32];
   char slot291[32];
   char slot293[32];
   char slot295[32];
   char slot297[32];
   char slot299[32];
   char slot301[32];
   char slot303[32];
   char slot305[32];
   char slot307[32];
   char slot309[32];
   char slot311[32];
   char slot313[32];
   char slot315[32];
   char slot317[32];
   char slot319[32];
   char slot320[32];
   char slot321[32];
   char slot322[32];
   char slot323[32];
   char slot324[32];
   char slot325[32];
   char slot326[32];
   char slot327[32];
   char slot328[32];
   char slot329[32];
   char slot330[32];
   char slot331[32];
   char slot332[32];
   char slot333[32];
   char slot334[32];
   char slot335[32];
   char slot336[32];
   char slot337[32];
   char slot338[32];
   char slot339[32];
   char slot340[32];
   char slot341[32];
   char slot342[32];
   char slot343[32];
   char slot344[32];
   char slot345[32];
   char slot346[32];
   char slot347[32];
   char slot348[32];
   char slot349[32];
   char slot350[32];
   char slot351[32];
   char slot352[32];
   char slot353[32];
   char slot354[32];
   char slot355[32];
   char slot356[32];
   char slot357[32];
   char slot358[32];
   char slot359[32];
   char slot360[32];
   char slot361[32];
   char slot362[32];
   char slot363[32];
   char slot364[32];
   char slot365[32];
   char slot366[32];
   char slot367[32];
   char slot368[32];
   char slot369[32];
   char slot370[32];
   char slot371[32];
   char slot372[32];
   char slot373[32];
   char slot374[32];
   char slot375[32];
   char slot376[32];
   char slot377[32];
   char slot378[32];
   char slot379[32];
   char slot380[32];
   char slot381[32];
   char slot382[32];
   char slot383[32];
   char slot384[32];
   char slot385[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->seen_paths[0], out->seen_paths[1], out->seen_paths[2], out->seen_paths[3], out->seen_paths[4], out->seen_paths[5], out->seen_paths[6], out->seen_paths[7], out->seen_paths[8], out->seen_paths[9], out->seen_paths[10], out->seen_paths[11], out->seen_paths[12], out->seen_paths[13], out->seen_paths[14], out->seen_paths[15], out->seen_paths[16], out->seen_paths[17], out->seen_paths[18], out->seen_paths[19], out->seen_paths[20], out->seen_paths[21], out->seen_paths[22], out->seen_paths[23], out->seen_paths[24], out->seen_paths[25], out->seen_paths[26], out->seen_paths[27], out->seen_paths[28], out->seen_paths[29], out->seen_paths[30], out->seen_paths[31], out->seen_paths[32], out->seen_paths[33], out->seen_paths[34], out->seen_paths[35], out->seen_paths[36], out->seen_paths[37], out->seen_paths[38], out->seen_paths[39], out->seen_paths[40], out->seen_paths[41], out->seen_paths[42], out->seen_paths[43], out->seen_paths[44], out->seen_paths[45], out->seen_paths[46], out->seen_paths[47], out->seen_paths[48], out->seen_paths[49], out->seen_paths[50], out->seen_paths[51], out->seen_paths[52], out->seen_paths[53], out->seen_paths[54], out->seen_paths[55], out->seen_paths[56], out->seen_paths[57], out->seen_paths[58], out->seen_paths[59], out->seen_paths[60], out->seen_paths[61], out->seen_paths[62], out->seen_paths[63], slot64, out->session_mode, out->guardrail_mode, slot67, slot68, slot69, out->worktrees[0].git_root, out->worktrees[0].worktree_path, out->worktrees[1].git_root, out->worktrees[1].worktree_path, out->worktrees[2].git_root, out->worktrees[2].worktree_path, out->worktrees[3].git_root, out->worktrees[3].worktree_path, out->worktrees[4].git_root, out->worktrees[4].worktree_path, out->worktrees[5].git_root, out->worktrees[5].worktree_path, out->worktrees[6].git_root, out->worktrees[6].worktree_path, out->worktrees[7].git_root, out->worktrees[7].worktree_path, out->worktrees[8].git_root, out->worktrees[8].worktree_path, out->worktrees[9].git_root, out->worktrees[9].worktree_path, out->worktrees[10].git_root, out->worktrees[10].worktree_path, out->worktrees[11].git_root, out->worktrees[11].worktree_path, out->worktrees[12].git_root, out->worktrees[12].worktree_path, out->worktrees[13].git_root, out->worktrees[13].worktree_path, out->worktrees[14].git_root, out->worktrees[14].worktree_path, out->worktrees[15].git_root, out->worktrees[15].worktree_path, slot102, slot103, slot104, slot105, slot106, slot107, slot108, out->tdd_mode, out->tdd_writes[0].stem, slot111, out->tdd_writes[1].stem, slot113, out->tdd_writes[2].stem, slot115, out->tdd_writes[3].stem, slot117, out->tdd_writes[4].stem, slot119, out->tdd_writes[5].stem, slot121, out->tdd_writes[6].stem, slot123, out->tdd_writes[7].stem, slot125, slot126, out->read_paths[0], out->read_paths[1], out->read_paths[2], out->read_paths[3], out->read_paths[4], out->read_paths[5], out->read_paths[6], out->read_paths[7], out->read_paths[8], out->read_paths[9], out->read_paths[10], out->read_paths[11], out->read_paths[12], out->read_paths[13], out->read_paths[14], out->read_paths[15], out->read_paths[16], out->read_paths[17], out->read_paths[18], out->read_paths[19], out->read_paths[20], out->read_paths[21], out->read_paths[22], out->read_paths[23], out->read_paths[24], out->read_paths[25], out->read_paths[26], out->read_paths[27], out->read_paths[28], out->read_paths[29], out->read_paths[30], out->read_paths[31], out->read_paths[32], out->read_paths[33], out->read_paths[34], out->read_paths[35], out->read_paths[36], out->read_paths[37], out->read_paths[38], out->read_paths[39], out->read_paths[40], out->read_paths[41], out->read_paths[42], out->read_paths[43], out->read_paths[44], out->read_paths[45], out->read_paths[46], out->read_paths[47], out->read_paths[48], out->read_paths[49], out->read_paths[50], out->read_paths[51], out->read_paths[52], out->read_paths[53], out->read_paths[54], out->read_paths[55], out->read_paths[56], out->read_paths[57], out->read_paths[58], out->read_paths[59], out->read_paths[60], out->read_paths[61], out->read_paths[62], out->read_paths[63], slot191, out->file_hashes[0].path, slot193, out->file_hashes[1].path, slot195, out->file_hashes[2].path, slot197, out->file_hashes[3].path, slot199, out->file_hashes[4].path, slot201, out->file_hashes[5].path, slot203, out->file_hashes[6].path, slot205, out->file_hashes[7].path, slot207, out->file_hashes[8].path, slot209, out->file_hashes[9].path, slot211, out->file_hashes[10].path, slot213, out->file_hashes[11].path, slot215, out->file_hashes[12].path, slot217, out->file_hashes[13].path, slot219, out->file_hashes[14].path, slot221, out->file_hashes[15].path, slot223, out->file_hashes[16].path, slot225, out->file_hashes[17].path, slot227, out->file_hashes[18].path, slot229, out->file_hashes[19].path, slot231, out->file_hashes[20].path, slot233, out->file_hashes[21].path, slot235, out->file_hashes[22].path, slot237, out->file_hashes[23].path, slot239, out->file_hashes[24].path, slot241, out->file_hashes[25].path, slot243, out->file_hashes[26].path, slot245, out->file_hashes[27].path, slot247, out->file_hashes[28].path, slot249, out->file_hashes[29].path, slot251, out->file_hashes[30].path, slot253, out->file_hashes[31].path, slot255, out->file_hashes[32].path, slot257, out->file_hashes[33].path, slot259, out->file_hashes[34].path, slot261, out->file_hashes[35].path, slot263, out->file_hashes[36].path, slot265, out->file_hashes[37].path, slot267, out->file_hashes[38].path, slot269, out->file_hashes[39].path, slot271, out->file_hashes[40].path, slot273, out->file_hashes[41].path, slot275, out->file_hashes[42].path, slot277, out->file_hashes[43].path, slot279, out->file_hashes[44].path, slot281, out->file_hashes[45].path, slot283, out->file_hashes[46].path, slot285, out->file_hashes[47].path, slot287, out->file_hashes[48].path, slot289, out->file_hashes[49].path, slot291, out->file_hashes[50].path, slot293, out->file_hashes[51].path, slot295, out->file_hashes[52].path, slot297, out->file_hashes[53].path, slot299, out->file_hashes[54].path, slot301, out->file_hashes[55].path, slot303, out->file_hashes[56].path, slot305, out->file_hashes[57].path, slot307, out->file_hashes[58].path, slot309, out->file_hashes[59].path, slot311, out->file_hashes[60].path, slot313, out->file_hashes[61].path, slot315, out->file_hashes[62].path, slot317, out->file_hashes[63].path, slot319, slot320, slot321, slot322, slot323, slot324, slot325, slot326, slot327, slot328, slot329, slot330, slot331, slot332, slot333, slot334, slot335, slot336, slot337, slot338, slot339, slot340, slot341, slot342, slot343, slot344, slot345, slot346, slot347, slot348, slot349, slot350, slot351, slot352, slot353, slot354, slot355, slot356, slot357, slot358, slot359, slot360, slot361, slot362, slot363, slot364, slot365, slot366, slot367, slot368, slot369, slot370, slot371, slot372, slot373, slot374, slot375, slot376, slot377, slot378, slot379, slot380, slot381, slot382, slot383, slot384, slot385};
   const size_t caps[] = {sizeof out->seen_paths[0], sizeof out->seen_paths[1], sizeof out->seen_paths[2], sizeof out->seen_paths[3], sizeof out->seen_paths[4], sizeof out->seen_paths[5], sizeof out->seen_paths[6], sizeof out->seen_paths[7], sizeof out->seen_paths[8], sizeof out->seen_paths[9], sizeof out->seen_paths[10], sizeof out->seen_paths[11], sizeof out->seen_paths[12], sizeof out->seen_paths[13], sizeof out->seen_paths[14], sizeof out->seen_paths[15], sizeof out->seen_paths[16], sizeof out->seen_paths[17], sizeof out->seen_paths[18], sizeof out->seen_paths[19], sizeof out->seen_paths[20], sizeof out->seen_paths[21], sizeof out->seen_paths[22], sizeof out->seen_paths[23], sizeof out->seen_paths[24], sizeof out->seen_paths[25], sizeof out->seen_paths[26], sizeof out->seen_paths[27], sizeof out->seen_paths[28], sizeof out->seen_paths[29], sizeof out->seen_paths[30], sizeof out->seen_paths[31], sizeof out->seen_paths[32], sizeof out->seen_paths[33], sizeof out->seen_paths[34], sizeof out->seen_paths[35], sizeof out->seen_paths[36], sizeof out->seen_paths[37], sizeof out->seen_paths[38], sizeof out->seen_paths[39], sizeof out->seen_paths[40], sizeof out->seen_paths[41], sizeof out->seen_paths[42], sizeof out->seen_paths[43], sizeof out->seen_paths[44], sizeof out->seen_paths[45], sizeof out->seen_paths[46], sizeof out->seen_paths[47], sizeof out->seen_paths[48], sizeof out->seen_paths[49], sizeof out->seen_paths[50], sizeof out->seen_paths[51], sizeof out->seen_paths[52], sizeof out->seen_paths[53], sizeof out->seen_paths[54], sizeof out->seen_paths[55], sizeof out->seen_paths[56], sizeof out->seen_paths[57], sizeof out->seen_paths[58], sizeof out->seen_paths[59], sizeof out->seen_paths[60], sizeof out->seen_paths[61], sizeof out->seen_paths[62], sizeof out->seen_paths[63], sizeof slot64, sizeof out->session_mode, sizeof out->guardrail_mode, sizeof slot67, sizeof slot68, sizeof slot69, sizeof out->worktrees[0].git_root, sizeof out->worktrees[0].worktree_path, sizeof out->worktrees[1].git_root, sizeof out->worktrees[1].worktree_path, sizeof out->worktrees[2].git_root, sizeof out->worktrees[2].worktree_path, sizeof out->worktrees[3].git_root, sizeof out->worktrees[3].worktree_path, sizeof out->worktrees[4].git_root, sizeof out->worktrees[4].worktree_path, sizeof out->worktrees[5].git_root, sizeof out->worktrees[5].worktree_path, sizeof out->worktrees[6].git_root, sizeof out->worktrees[6].worktree_path, sizeof out->worktrees[7].git_root, sizeof out->worktrees[7].worktree_path, sizeof out->worktrees[8].git_root, sizeof out->worktrees[8].worktree_path, sizeof out->worktrees[9].git_root, sizeof out->worktrees[9].worktree_path, sizeof out->worktrees[10].git_root, sizeof out->worktrees[10].worktree_path, sizeof out->worktrees[11].git_root, sizeof out->worktrees[11].worktree_path, sizeof out->worktrees[12].git_root, sizeof out->worktrees[12].worktree_path, sizeof out->worktrees[13].git_root, sizeof out->worktrees[13].worktree_path, sizeof out->worktrees[14].git_root, sizeof out->worktrees[14].worktree_path, sizeof out->worktrees[15].git_root, sizeof out->worktrees[15].worktree_path, sizeof slot102, sizeof slot103, sizeof slot104, sizeof slot105, sizeof slot106, sizeof slot107, sizeof slot108, sizeof out->tdd_mode, sizeof out->tdd_writes[0].stem, sizeof slot111, sizeof out->tdd_writes[1].stem, sizeof slot113, sizeof out->tdd_writes[2].stem, sizeof slot115, sizeof out->tdd_writes[3].stem, sizeof slot117, sizeof out->tdd_writes[4].stem, sizeof slot119, sizeof out->tdd_writes[5].stem, sizeof slot121, sizeof out->tdd_writes[6].stem, sizeof slot123, sizeof out->tdd_writes[7].stem, sizeof slot125, sizeof slot126, sizeof out->read_paths[0], sizeof out->read_paths[1], sizeof out->read_paths[2], sizeof out->read_paths[3], sizeof out->read_paths[4], sizeof out->read_paths[5], sizeof out->read_paths[6], sizeof out->read_paths[7], sizeof out->read_paths[8], sizeof out->read_paths[9], sizeof out->read_paths[10], sizeof out->read_paths[11], sizeof out->read_paths[12], sizeof out->read_paths[13], sizeof out->read_paths[14], sizeof out->read_paths[15], sizeof out->read_paths[16], sizeof out->read_paths[17], sizeof out->read_paths[18], sizeof out->read_paths[19], sizeof out->read_paths[20], sizeof out->read_paths[21], sizeof out->read_paths[22], sizeof out->read_paths[23], sizeof out->read_paths[24], sizeof out->read_paths[25], sizeof out->read_paths[26], sizeof out->read_paths[27], sizeof out->read_paths[28], sizeof out->read_paths[29], sizeof out->read_paths[30], sizeof out->read_paths[31], sizeof out->read_paths[32], sizeof out->read_paths[33], sizeof out->read_paths[34], sizeof out->read_paths[35], sizeof out->read_paths[36], sizeof out->read_paths[37], sizeof out->read_paths[38], sizeof out->read_paths[39], sizeof out->read_paths[40], sizeof out->read_paths[41], sizeof out->read_paths[42], sizeof out->read_paths[43], sizeof out->read_paths[44], sizeof out->read_paths[45], sizeof out->read_paths[46], sizeof out->read_paths[47], sizeof out->read_paths[48], sizeof out->read_paths[49], sizeof out->read_paths[50], sizeof out->read_paths[51], sizeof out->read_paths[52], sizeof out->read_paths[53], sizeof out->read_paths[54], sizeof out->read_paths[55], sizeof out->read_paths[56], sizeof out->read_paths[57], sizeof out->read_paths[58], sizeof out->read_paths[59], sizeof out->read_paths[60], sizeof out->read_paths[61], sizeof out->read_paths[62], sizeof out->read_paths[63], sizeof slot191, sizeof out->file_hashes[0].path, sizeof slot193, sizeof out->file_hashes[1].path, sizeof slot195, sizeof out->file_hashes[2].path, sizeof slot197, sizeof out->file_hashes[3].path, sizeof slot199, sizeof out->file_hashes[4].path, sizeof slot201, sizeof out->file_hashes[5].path, sizeof slot203, sizeof out->file_hashes[6].path, sizeof slot205, sizeof out->file_hashes[7].path, sizeof slot207, sizeof out->file_hashes[8].path, sizeof slot209, sizeof out->file_hashes[9].path, sizeof slot211, sizeof out->file_hashes[10].path, sizeof slot213, sizeof out->file_hashes[11].path, sizeof slot215, sizeof out->file_hashes[12].path, sizeof slot217, sizeof out->file_hashes[13].path, sizeof slot219, sizeof out->file_hashes[14].path, sizeof slot221, sizeof out->file_hashes[15].path, sizeof slot223, sizeof out->file_hashes[16].path, sizeof slot225, sizeof out->file_hashes[17].path, sizeof slot227, sizeof out->file_hashes[18].path, sizeof slot229, sizeof out->file_hashes[19].path, sizeof slot231, sizeof out->file_hashes[20].path, sizeof slot233, sizeof out->file_hashes[21].path, sizeof slot235, sizeof out->file_hashes[22].path, sizeof slot237, sizeof out->file_hashes[23].path, sizeof slot239, sizeof out->file_hashes[24].path, sizeof slot241, sizeof out->file_hashes[25].path, sizeof slot243, sizeof out->file_hashes[26].path, sizeof slot245, sizeof out->file_hashes[27].path, sizeof slot247, sizeof out->file_hashes[28].path, sizeof slot249, sizeof out->file_hashes[29].path, sizeof slot251, sizeof out->file_hashes[30].path, sizeof slot253, sizeof out->file_hashes[31].path, sizeof slot255, sizeof out->file_hashes[32].path, sizeof slot257, sizeof out->file_hashes[33].path, sizeof slot259, sizeof out->file_hashes[34].path, sizeof slot261, sizeof out->file_hashes[35].path, sizeof slot263, sizeof out->file_hashes[36].path, sizeof slot265, sizeof out->file_hashes[37].path, sizeof slot267, sizeof out->file_hashes[38].path, sizeof slot269, sizeof out->file_hashes[39].path, sizeof slot271, sizeof out->file_hashes[40].path, sizeof slot273, sizeof out->file_hashes[41].path, sizeof slot275, sizeof out->file_hashes[42].path, sizeof slot277, sizeof out->file_hashes[43].path, sizeof slot279, sizeof out->file_hashes[44].path, sizeof slot281, sizeof out->file_hashes[45].path, sizeof slot283, sizeof out->file_hashes[46].path, sizeof slot285, sizeof out->file_hashes[47].path, sizeof slot287, sizeof out->file_hashes[48].path, sizeof slot289, sizeof out->file_hashes[49].path, sizeof slot291, sizeof out->file_hashes[50].path, sizeof slot293, sizeof out->file_hashes[51].path, sizeof slot295, sizeof out->file_hashes[52].path, sizeof slot297, sizeof out->file_hashes[53].path, sizeof slot299, sizeof out->file_hashes[54].path, sizeof slot301, sizeof out->file_hashes[55].path, sizeof slot303, sizeof out->file_hashes[56].path, sizeof slot305, sizeof out->file_hashes[57].path, sizeof slot307, sizeof out->file_hashes[58].path, sizeof slot309, sizeof out->file_hashes[59].path, sizeof slot311, sizeof out->file_hashes[60].path, sizeof slot313, sizeof out->file_hashes[61].path, sizeof slot315, sizeof out->file_hashes[62].path, sizeof slot317, sizeof out->file_hashes[63].path, sizeof slot319, sizeof slot320, sizeof slot321, sizeof slot322, sizeof slot323, sizeof slot324, sizeof slot325, sizeof slot326, sizeof slot327, sizeof slot328, sizeof slot329, sizeof slot330, sizeof slot331, sizeof slot332, sizeof slot333, sizeof slot334, sizeof slot335, sizeof slot336, sizeof slot337, sizeof slot338, sizeof slot339, sizeof slot340, sizeof slot341, sizeof slot342, sizeof slot343, sizeof slot344, sizeof slot345, sizeof slot346, sizeof slot347, sizeof slot348, sizeof slot349, sizeof slot350, sizeof slot351, sizeof slot352, sizeof slot353, sizeof slot354, sizeof slot355, sizeof slot356, sizeof slot357, sizeof slot358, sizeof slot359, sizeof slot360, sizeof slot361, sizeof slot362, sizeof slot363, sizeof slot364, sizeof slot365, sizeof slot366, sizeof slot367, sizeof slot368, sizeof slot369, sizeof slot370, sizeof slot371, sizeof slot372, sizeof slot373, sizeof slot374, sizeof slot375, sizeof slot376, sizeof slot377, sizeof slot378, sizeof slot379, sizeof slot380, sizeof slot381, sizeof slot382, sizeof slot383, sizeof slot384, sizeof slot385};
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STATE_LOAD, fields, 1, values, caps, 386, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->seen_count = (int)strtol(slot64, NULL, 10);
   out->active_task_id = (int64_t)strtoll(slot67, NULL, 10);
   out->hook_call_count = (int)strtol(slot68, NULL, 10);
   out->dirty = (int)strtol(slot69, NULL, 10);
   out->worktree_count = (int)strtol(slot102, NULL, 10);
   out->is_delegate = (int)strtol(slot103, NULL, 10);
   out->orch_direct_edits = (int)strtol(slot104, NULL, 10);
   out->orch_nudge_sent = (int)strtol(slot105, NULL, 10);
   out->skill_find_symbols_advisory_sent = (int)strtol(slot106, NULL, 10);
   out->skill_condition_waiting_advisory_sent = (int)strtol(slot107, NULL, 10);
   out->skill_tdd_advisory_sent = (int)strtol(slot108, NULL, 10);
   out->tdd_writes[0].is_test = (int)strtol(slot111, NULL, 10);
   out->tdd_writes[1].is_test = (int)strtol(slot113, NULL, 10);
   out->tdd_writes[2].is_test = (int)strtol(slot115, NULL, 10);
   out->tdd_writes[3].is_test = (int)strtol(slot117, NULL, 10);
   out->tdd_writes[4].is_test = (int)strtol(slot119, NULL, 10);
   out->tdd_writes[5].is_test = (int)strtol(slot121, NULL, 10);
   out->tdd_writes[6].is_test = (int)strtol(slot123, NULL, 10);
   out->tdd_writes[7].is_test = (int)strtol(slot125, NULL, 10);
   out->tdd_write_count = (int)strtol(slot126, NULL, 10);
   out->read_path_count = (int)strtol(slot191, NULL, 10);
   out->file_hashes[0].content_hash = (uint64_t)strtoull(slot193, NULL, 10);
   out->file_hashes[1].content_hash = (uint64_t)strtoull(slot195, NULL, 10);
   out->file_hashes[2].content_hash = (uint64_t)strtoull(slot197, NULL, 10);
   out->file_hashes[3].content_hash = (uint64_t)strtoull(slot199, NULL, 10);
   out->file_hashes[4].content_hash = (uint64_t)strtoull(slot201, NULL, 10);
   out->file_hashes[5].content_hash = (uint64_t)strtoull(slot203, NULL, 10);
   out->file_hashes[6].content_hash = (uint64_t)strtoull(slot205, NULL, 10);
   out->file_hashes[7].content_hash = (uint64_t)strtoull(slot207, NULL, 10);
   out->file_hashes[8].content_hash = (uint64_t)strtoull(slot209, NULL, 10);
   out->file_hashes[9].content_hash = (uint64_t)strtoull(slot211, NULL, 10);
   out->file_hashes[10].content_hash = (uint64_t)strtoull(slot213, NULL, 10);
   out->file_hashes[11].content_hash = (uint64_t)strtoull(slot215, NULL, 10);
   out->file_hashes[12].content_hash = (uint64_t)strtoull(slot217, NULL, 10);
   out->file_hashes[13].content_hash = (uint64_t)strtoull(slot219, NULL, 10);
   out->file_hashes[14].content_hash = (uint64_t)strtoull(slot221, NULL, 10);
   out->file_hashes[15].content_hash = (uint64_t)strtoull(slot223, NULL, 10);
   out->file_hashes[16].content_hash = (uint64_t)strtoull(slot225, NULL, 10);
   out->file_hashes[17].content_hash = (uint64_t)strtoull(slot227, NULL, 10);
   out->file_hashes[18].content_hash = (uint64_t)strtoull(slot229, NULL, 10);
   out->file_hashes[19].content_hash = (uint64_t)strtoull(slot231, NULL, 10);
   out->file_hashes[20].content_hash = (uint64_t)strtoull(slot233, NULL, 10);
   out->file_hashes[21].content_hash = (uint64_t)strtoull(slot235, NULL, 10);
   out->file_hashes[22].content_hash = (uint64_t)strtoull(slot237, NULL, 10);
   out->file_hashes[23].content_hash = (uint64_t)strtoull(slot239, NULL, 10);
   out->file_hashes[24].content_hash = (uint64_t)strtoull(slot241, NULL, 10);
   out->file_hashes[25].content_hash = (uint64_t)strtoull(slot243, NULL, 10);
   out->file_hashes[26].content_hash = (uint64_t)strtoull(slot245, NULL, 10);
   out->file_hashes[27].content_hash = (uint64_t)strtoull(slot247, NULL, 10);
   out->file_hashes[28].content_hash = (uint64_t)strtoull(slot249, NULL, 10);
   out->file_hashes[29].content_hash = (uint64_t)strtoull(slot251, NULL, 10);
   out->file_hashes[30].content_hash = (uint64_t)strtoull(slot253, NULL, 10);
   out->file_hashes[31].content_hash = (uint64_t)strtoull(slot255, NULL, 10);
   out->file_hashes[32].content_hash = (uint64_t)strtoull(slot257, NULL, 10);
   out->file_hashes[33].content_hash = (uint64_t)strtoull(slot259, NULL, 10);
   out->file_hashes[34].content_hash = (uint64_t)strtoull(slot261, NULL, 10);
   out->file_hashes[35].content_hash = (uint64_t)strtoull(slot263, NULL, 10);
   out->file_hashes[36].content_hash = (uint64_t)strtoull(slot265, NULL, 10);
   out->file_hashes[37].content_hash = (uint64_t)strtoull(slot267, NULL, 10);
   out->file_hashes[38].content_hash = (uint64_t)strtoull(slot269, NULL, 10);
   out->file_hashes[39].content_hash = (uint64_t)strtoull(slot271, NULL, 10);
   out->file_hashes[40].content_hash = (uint64_t)strtoull(slot273, NULL, 10);
   out->file_hashes[41].content_hash = (uint64_t)strtoull(slot275, NULL, 10);
   out->file_hashes[42].content_hash = (uint64_t)strtoull(slot277, NULL, 10);
   out->file_hashes[43].content_hash = (uint64_t)strtoull(slot279, NULL, 10);
   out->file_hashes[44].content_hash = (uint64_t)strtoull(slot281, NULL, 10);
   out->file_hashes[45].content_hash = (uint64_t)strtoull(slot283, NULL, 10);
   out->file_hashes[46].content_hash = (uint64_t)strtoull(slot285, NULL, 10);
   out->file_hashes[47].content_hash = (uint64_t)strtoull(slot287, NULL, 10);
   out->file_hashes[48].content_hash = (uint64_t)strtoull(slot289, NULL, 10);
   out->file_hashes[49].content_hash = (uint64_t)strtoull(slot291, NULL, 10);
   out->file_hashes[50].content_hash = (uint64_t)strtoull(slot293, NULL, 10);
   out->file_hashes[51].content_hash = (uint64_t)strtoull(slot295, NULL, 10);
   out->file_hashes[52].content_hash = (uint64_t)strtoull(slot297, NULL, 10);
   out->file_hashes[53].content_hash = (uint64_t)strtoull(slot299, NULL, 10);
   out->file_hashes[54].content_hash = (uint64_t)strtoull(slot301, NULL, 10);
   out->file_hashes[55].content_hash = (uint64_t)strtoull(slot303, NULL, 10);
   out->file_hashes[56].content_hash = (uint64_t)strtoull(slot305, NULL, 10);
   out->file_hashes[57].content_hash = (uint64_t)strtoull(slot307, NULL, 10);
   out->file_hashes[58].content_hash = (uint64_t)strtoull(slot309, NULL, 10);
   out->file_hashes[59].content_hash = (uint64_t)strtoull(slot311, NULL, 10);
   out->file_hashes[60].content_hash = (uint64_t)strtoull(slot313, NULL, 10);
   out->file_hashes[61].content_hash = (uint64_t)strtoull(slot315, NULL, 10);
   out->file_hashes[62].content_hash = (uint64_t)strtoull(slot317, NULL, 10);
   out->file_hashes[63].content_hash = (uint64_t)strtoull(slot319, NULL, 10);
   out->file_hash_count = (int)strtol(slot320, NULL, 10);
   out->ap_hits[0].pattern_id = (int64_t)strtoll(slot321, NULL, 10);
   out->ap_hits[0].hits = (int)strtol(slot322, NULL, 10);
   out->ap_hits[1].pattern_id = (int64_t)strtoll(slot323, NULL, 10);
   out->ap_hits[1].hits = (int)strtol(slot324, NULL, 10);
   out->ap_hits[2].pattern_id = (int64_t)strtoll(slot325, NULL, 10);
   out->ap_hits[2].hits = (int)strtol(slot326, NULL, 10);
   out->ap_hits[3].pattern_id = (int64_t)strtoll(slot327, NULL, 10);
   out->ap_hits[3].hits = (int)strtol(slot328, NULL, 10);
   out->ap_hits[4].pattern_id = (int64_t)strtoll(slot329, NULL, 10);
   out->ap_hits[4].hits = (int)strtol(slot330, NULL, 10);
   out->ap_hits[5].pattern_id = (int64_t)strtoll(slot331, NULL, 10);
   out->ap_hits[5].hits = (int)strtol(slot332, NULL, 10);
   out->ap_hits[6].pattern_id = (int64_t)strtoll(slot333, NULL, 10);
   out->ap_hits[6].hits = (int)strtol(slot334, NULL, 10);
   out->ap_hits[7].pattern_id = (int64_t)strtoll(slot335, NULL, 10);
   out->ap_hits[7].hits = (int)strtol(slot336, NULL, 10);
   out->ap_hits[8].pattern_id = (int64_t)strtoll(slot337, NULL, 10);
   out->ap_hits[8].hits = (int)strtol(slot338, NULL, 10);
   out->ap_hits[9].pattern_id = (int64_t)strtoll(slot339, NULL, 10);
   out->ap_hits[9].hits = (int)strtol(slot340, NULL, 10);
   out->ap_hits[10].pattern_id = (int64_t)strtoll(slot341, NULL, 10);
   out->ap_hits[10].hits = (int)strtol(slot342, NULL, 10);
   out->ap_hits[11].pattern_id = (int64_t)strtoll(slot343, NULL, 10);
   out->ap_hits[11].hits = (int)strtol(slot344, NULL, 10);
   out->ap_hits[12].pattern_id = (int64_t)strtoll(slot345, NULL, 10);
   out->ap_hits[12].hits = (int)strtol(slot346, NULL, 10);
   out->ap_hits[13].pattern_id = (int64_t)strtoll(slot347, NULL, 10);
   out->ap_hits[13].hits = (int)strtol(slot348, NULL, 10);
   out->ap_hits[14].pattern_id = (int64_t)strtoll(slot349, NULL, 10);
   out->ap_hits[14].hits = (int)strtol(slot350, NULL, 10);
   out->ap_hits[15].pattern_id = (int64_t)strtoll(slot351, NULL, 10);
   out->ap_hits[15].hits = (int)strtol(slot352, NULL, 10);
   out->ap_hits[16].pattern_id = (int64_t)strtoll(slot353, NULL, 10);
   out->ap_hits[16].hits = (int)strtol(slot354, NULL, 10);
   out->ap_hits[17].pattern_id = (int64_t)strtoll(slot355, NULL, 10);
   out->ap_hits[17].hits = (int)strtol(slot356, NULL, 10);
   out->ap_hits[18].pattern_id = (int64_t)strtoll(slot357, NULL, 10);
   out->ap_hits[18].hits = (int)strtol(slot358, NULL, 10);
   out->ap_hits[19].pattern_id = (int64_t)strtoll(slot359, NULL, 10);
   out->ap_hits[19].hits = (int)strtol(slot360, NULL, 10);
   out->ap_hits[20].pattern_id = (int64_t)strtoll(slot361, NULL, 10);
   out->ap_hits[20].hits = (int)strtol(slot362, NULL, 10);
   out->ap_hits[21].pattern_id = (int64_t)strtoll(slot363, NULL, 10);
   out->ap_hits[21].hits = (int)strtol(slot364, NULL, 10);
   out->ap_hits[22].pattern_id = (int64_t)strtoll(slot365, NULL, 10);
   out->ap_hits[22].hits = (int)strtol(slot366, NULL, 10);
   out->ap_hits[23].pattern_id = (int64_t)strtoll(slot367, NULL, 10);
   out->ap_hits[23].hits = (int)strtol(slot368, NULL, 10);
   out->ap_hits[24].pattern_id = (int64_t)strtoll(slot369, NULL, 10);
   out->ap_hits[24].hits = (int)strtol(slot370, NULL, 10);
   out->ap_hits[25].pattern_id = (int64_t)strtoll(slot371, NULL, 10);
   out->ap_hits[25].hits = (int)strtol(slot372, NULL, 10);
   out->ap_hits[26].pattern_id = (int64_t)strtoll(slot373, NULL, 10);
   out->ap_hits[26].hits = (int)strtol(slot374, NULL, 10);
   out->ap_hits[27].pattern_id = (int64_t)strtoll(slot375, NULL, 10);
   out->ap_hits[27].hits = (int)strtol(slot376, NULL, 10);
   out->ap_hits[28].pattern_id = (int64_t)strtoll(slot377, NULL, 10);
   out->ap_hits[28].hits = (int)strtol(slot378, NULL, 10);
   out->ap_hits[29].pattern_id = (int64_t)strtoll(slot379, NULL, 10);
   out->ap_hits[29].hits = (int)strtol(slot380, NULL, 10);
   out->ap_hits[30].pattern_id = (int64_t)strtoll(slot381, NULL, 10);
   out->ap_hits[30].hits = (int)strtol(slot382, NULL, 10);
   out->ap_hits[31].pattern_id = (int64_t)strtoll(slot383, NULL, 10);
   out->ap_hits[31].hits = (int)strtol(slot384, NULL, 10);
   out->ap_hit_count = (int)strtol(slot385, NULL, 10);
   return 0;
}

int db1_session_state_save(const char *sid, const session_state_t *in)
{
   if (!sid || !sid[0] || !in)
      return -1;
   char arg65[32];
   snprintf(arg65, sizeof arg65, "%d", in->seen_count);
   char arg68[32];
   snprintf(arg68, sizeof arg68, "%lld", (long long)in->active_task_id);
   char arg69[32];
   snprintf(arg69, sizeof arg69, "%d", in->hook_call_count);
   char arg70[32];
   snprintf(arg70, sizeof arg70, "%d", in->dirty);
   char arg103[32];
   snprintf(arg103, sizeof arg103, "%d", in->worktree_count);
   char arg104[32];
   snprintf(arg104, sizeof arg104, "%d", in->is_delegate);
   char arg105[32];
   snprintf(arg105, sizeof arg105, "%d", in->orch_direct_edits);
   char arg106[32];
   snprintf(arg106, sizeof arg106, "%d", in->orch_nudge_sent);
   char arg107[32];
   snprintf(arg107, sizeof arg107, "%d", in->skill_find_symbols_advisory_sent);
   char arg108[32];
   snprintf(arg108, sizeof arg108, "%d", in->skill_condition_waiting_advisory_sent);
   char arg109[32];
   snprintf(arg109, sizeof arg109, "%d", in->skill_tdd_advisory_sent);
   char arg112[32];
   snprintf(arg112, sizeof arg112, "%d", in->tdd_writes[0].is_test);
   char arg114[32];
   snprintf(arg114, sizeof arg114, "%d", in->tdd_writes[1].is_test);
   char arg116[32];
   snprintf(arg116, sizeof arg116, "%d", in->tdd_writes[2].is_test);
   char arg118[32];
   snprintf(arg118, sizeof arg118, "%d", in->tdd_writes[3].is_test);
   char arg120[32];
   snprintf(arg120, sizeof arg120, "%d", in->tdd_writes[4].is_test);
   char arg122[32];
   snprintf(arg122, sizeof arg122, "%d", in->tdd_writes[5].is_test);
   char arg124[32];
   snprintf(arg124, sizeof arg124, "%d", in->tdd_writes[6].is_test);
   char arg126[32];
   snprintf(arg126, sizeof arg126, "%d", in->tdd_writes[7].is_test);
   char arg127[32];
   snprintf(arg127, sizeof arg127, "%d", in->tdd_write_count);
   char arg192[32];
   snprintf(arg192, sizeof arg192, "%d", in->read_path_count);
   char arg194[32];
   snprintf(arg194, sizeof arg194, "%llu", (unsigned long long)in->file_hashes[0].content_hash);
   char arg196[32];
   snprintf(arg196, sizeof arg196, "%llu", (unsigned long long)in->file_hashes[1].content_hash);
   char arg198[32];
   snprintf(arg198, sizeof arg198, "%llu", (unsigned long long)in->file_hashes[2].content_hash);
   char arg200[32];
   snprintf(arg200, sizeof arg200, "%llu", (unsigned long long)in->file_hashes[3].content_hash);
   char arg202[32];
   snprintf(arg202, sizeof arg202, "%llu", (unsigned long long)in->file_hashes[4].content_hash);
   char arg204[32];
   snprintf(arg204, sizeof arg204, "%llu", (unsigned long long)in->file_hashes[5].content_hash);
   char arg206[32];
   snprintf(arg206, sizeof arg206, "%llu", (unsigned long long)in->file_hashes[6].content_hash);
   char arg208[32];
   snprintf(arg208, sizeof arg208, "%llu", (unsigned long long)in->file_hashes[7].content_hash);
   char arg210[32];
   snprintf(arg210, sizeof arg210, "%llu", (unsigned long long)in->file_hashes[8].content_hash);
   char arg212[32];
   snprintf(arg212, sizeof arg212, "%llu", (unsigned long long)in->file_hashes[9].content_hash);
   char arg214[32];
   snprintf(arg214, sizeof arg214, "%llu", (unsigned long long)in->file_hashes[10].content_hash);
   char arg216[32];
   snprintf(arg216, sizeof arg216, "%llu", (unsigned long long)in->file_hashes[11].content_hash);
   char arg218[32];
   snprintf(arg218, sizeof arg218, "%llu", (unsigned long long)in->file_hashes[12].content_hash);
   char arg220[32];
   snprintf(arg220, sizeof arg220, "%llu", (unsigned long long)in->file_hashes[13].content_hash);
   char arg222[32];
   snprintf(arg222, sizeof arg222, "%llu", (unsigned long long)in->file_hashes[14].content_hash);
   char arg224[32];
   snprintf(arg224, sizeof arg224, "%llu", (unsigned long long)in->file_hashes[15].content_hash);
   char arg226[32];
   snprintf(arg226, sizeof arg226, "%llu", (unsigned long long)in->file_hashes[16].content_hash);
   char arg228[32];
   snprintf(arg228, sizeof arg228, "%llu", (unsigned long long)in->file_hashes[17].content_hash);
   char arg230[32];
   snprintf(arg230, sizeof arg230, "%llu", (unsigned long long)in->file_hashes[18].content_hash);
   char arg232[32];
   snprintf(arg232, sizeof arg232, "%llu", (unsigned long long)in->file_hashes[19].content_hash);
   char arg234[32];
   snprintf(arg234, sizeof arg234, "%llu", (unsigned long long)in->file_hashes[20].content_hash);
   char arg236[32];
   snprintf(arg236, sizeof arg236, "%llu", (unsigned long long)in->file_hashes[21].content_hash);
   char arg238[32];
   snprintf(arg238, sizeof arg238, "%llu", (unsigned long long)in->file_hashes[22].content_hash);
   char arg240[32];
   snprintf(arg240, sizeof arg240, "%llu", (unsigned long long)in->file_hashes[23].content_hash);
   char arg242[32];
   snprintf(arg242, sizeof arg242, "%llu", (unsigned long long)in->file_hashes[24].content_hash);
   char arg244[32];
   snprintf(arg244, sizeof arg244, "%llu", (unsigned long long)in->file_hashes[25].content_hash);
   char arg246[32];
   snprintf(arg246, sizeof arg246, "%llu", (unsigned long long)in->file_hashes[26].content_hash);
   char arg248[32];
   snprintf(arg248, sizeof arg248, "%llu", (unsigned long long)in->file_hashes[27].content_hash);
   char arg250[32];
   snprintf(arg250, sizeof arg250, "%llu", (unsigned long long)in->file_hashes[28].content_hash);
   char arg252[32];
   snprintf(arg252, sizeof arg252, "%llu", (unsigned long long)in->file_hashes[29].content_hash);
   char arg254[32];
   snprintf(arg254, sizeof arg254, "%llu", (unsigned long long)in->file_hashes[30].content_hash);
   char arg256[32];
   snprintf(arg256, sizeof arg256, "%llu", (unsigned long long)in->file_hashes[31].content_hash);
   char arg258[32];
   snprintf(arg258, sizeof arg258, "%llu", (unsigned long long)in->file_hashes[32].content_hash);
   char arg260[32];
   snprintf(arg260, sizeof arg260, "%llu", (unsigned long long)in->file_hashes[33].content_hash);
   char arg262[32];
   snprintf(arg262, sizeof arg262, "%llu", (unsigned long long)in->file_hashes[34].content_hash);
   char arg264[32];
   snprintf(arg264, sizeof arg264, "%llu", (unsigned long long)in->file_hashes[35].content_hash);
   char arg266[32];
   snprintf(arg266, sizeof arg266, "%llu", (unsigned long long)in->file_hashes[36].content_hash);
   char arg268[32];
   snprintf(arg268, sizeof arg268, "%llu", (unsigned long long)in->file_hashes[37].content_hash);
   char arg270[32];
   snprintf(arg270, sizeof arg270, "%llu", (unsigned long long)in->file_hashes[38].content_hash);
   char arg272[32];
   snprintf(arg272, sizeof arg272, "%llu", (unsigned long long)in->file_hashes[39].content_hash);
   char arg274[32];
   snprintf(arg274, sizeof arg274, "%llu", (unsigned long long)in->file_hashes[40].content_hash);
   char arg276[32];
   snprintf(arg276, sizeof arg276, "%llu", (unsigned long long)in->file_hashes[41].content_hash);
   char arg278[32];
   snprintf(arg278, sizeof arg278, "%llu", (unsigned long long)in->file_hashes[42].content_hash);
   char arg280[32];
   snprintf(arg280, sizeof arg280, "%llu", (unsigned long long)in->file_hashes[43].content_hash);
   char arg282[32];
   snprintf(arg282, sizeof arg282, "%llu", (unsigned long long)in->file_hashes[44].content_hash);
   char arg284[32];
   snprintf(arg284, sizeof arg284, "%llu", (unsigned long long)in->file_hashes[45].content_hash);
   char arg286[32];
   snprintf(arg286, sizeof arg286, "%llu", (unsigned long long)in->file_hashes[46].content_hash);
   char arg288[32];
   snprintf(arg288, sizeof arg288, "%llu", (unsigned long long)in->file_hashes[47].content_hash);
   char arg290[32];
   snprintf(arg290, sizeof arg290, "%llu", (unsigned long long)in->file_hashes[48].content_hash);
   char arg292[32];
   snprintf(arg292, sizeof arg292, "%llu", (unsigned long long)in->file_hashes[49].content_hash);
   char arg294[32];
   snprintf(arg294, sizeof arg294, "%llu", (unsigned long long)in->file_hashes[50].content_hash);
   char arg296[32];
   snprintf(arg296, sizeof arg296, "%llu", (unsigned long long)in->file_hashes[51].content_hash);
   char arg298[32];
   snprintf(arg298, sizeof arg298, "%llu", (unsigned long long)in->file_hashes[52].content_hash);
   char arg300[32];
   snprintf(arg300, sizeof arg300, "%llu", (unsigned long long)in->file_hashes[53].content_hash);
   char arg302[32];
   snprintf(arg302, sizeof arg302, "%llu", (unsigned long long)in->file_hashes[54].content_hash);
   char arg304[32];
   snprintf(arg304, sizeof arg304, "%llu", (unsigned long long)in->file_hashes[55].content_hash);
   char arg306[32];
   snprintf(arg306, sizeof arg306, "%llu", (unsigned long long)in->file_hashes[56].content_hash);
   char arg308[32];
   snprintf(arg308, sizeof arg308, "%llu", (unsigned long long)in->file_hashes[57].content_hash);
   char arg310[32];
   snprintf(arg310, sizeof arg310, "%llu", (unsigned long long)in->file_hashes[58].content_hash);
   char arg312[32];
   snprintf(arg312, sizeof arg312, "%llu", (unsigned long long)in->file_hashes[59].content_hash);
   char arg314[32];
   snprintf(arg314, sizeof arg314, "%llu", (unsigned long long)in->file_hashes[60].content_hash);
   char arg316[32];
   snprintf(arg316, sizeof arg316, "%llu", (unsigned long long)in->file_hashes[61].content_hash);
   char arg318[32];
   snprintf(arg318, sizeof arg318, "%llu", (unsigned long long)in->file_hashes[62].content_hash);
   char arg320[32];
   snprintf(arg320, sizeof arg320, "%llu", (unsigned long long)in->file_hashes[63].content_hash);
   char arg321[32];
   snprintf(arg321, sizeof arg321, "%d", in->file_hash_count);
   char arg322[32];
   snprintf(arg322, sizeof arg322, "%lld", (long long)in->ap_hits[0].pattern_id);
   char arg323[32];
   snprintf(arg323, sizeof arg323, "%d", in->ap_hits[0].hits);
   char arg324[32];
   snprintf(arg324, sizeof arg324, "%lld", (long long)in->ap_hits[1].pattern_id);
   char arg325[32];
   snprintf(arg325, sizeof arg325, "%d", in->ap_hits[1].hits);
   char arg326[32];
   snprintf(arg326, sizeof arg326, "%lld", (long long)in->ap_hits[2].pattern_id);
   char arg327[32];
   snprintf(arg327, sizeof arg327, "%d", in->ap_hits[2].hits);
   char arg328[32];
   snprintf(arg328, sizeof arg328, "%lld", (long long)in->ap_hits[3].pattern_id);
   char arg329[32];
   snprintf(arg329, sizeof arg329, "%d", in->ap_hits[3].hits);
   char arg330[32];
   snprintf(arg330, sizeof arg330, "%lld", (long long)in->ap_hits[4].pattern_id);
   char arg331[32];
   snprintf(arg331, sizeof arg331, "%d", in->ap_hits[4].hits);
   char arg332[32];
   snprintf(arg332, sizeof arg332, "%lld", (long long)in->ap_hits[5].pattern_id);
   char arg333[32];
   snprintf(arg333, sizeof arg333, "%d", in->ap_hits[5].hits);
   char arg334[32];
   snprintf(arg334, sizeof arg334, "%lld", (long long)in->ap_hits[6].pattern_id);
   char arg335[32];
   snprintf(arg335, sizeof arg335, "%d", in->ap_hits[6].hits);
   char arg336[32];
   snprintf(arg336, sizeof arg336, "%lld", (long long)in->ap_hits[7].pattern_id);
   char arg337[32];
   snprintf(arg337, sizeof arg337, "%d", in->ap_hits[7].hits);
   char arg338[32];
   snprintf(arg338, sizeof arg338, "%lld", (long long)in->ap_hits[8].pattern_id);
   char arg339[32];
   snprintf(arg339, sizeof arg339, "%d", in->ap_hits[8].hits);
   char arg340[32];
   snprintf(arg340, sizeof arg340, "%lld", (long long)in->ap_hits[9].pattern_id);
   char arg341[32];
   snprintf(arg341, sizeof arg341, "%d", in->ap_hits[9].hits);
   char arg342[32];
   snprintf(arg342, sizeof arg342, "%lld", (long long)in->ap_hits[10].pattern_id);
   char arg343[32];
   snprintf(arg343, sizeof arg343, "%d", in->ap_hits[10].hits);
   char arg344[32];
   snprintf(arg344, sizeof arg344, "%lld", (long long)in->ap_hits[11].pattern_id);
   char arg345[32];
   snprintf(arg345, sizeof arg345, "%d", in->ap_hits[11].hits);
   char arg346[32];
   snprintf(arg346, sizeof arg346, "%lld", (long long)in->ap_hits[12].pattern_id);
   char arg347[32];
   snprintf(arg347, sizeof arg347, "%d", in->ap_hits[12].hits);
   char arg348[32];
   snprintf(arg348, sizeof arg348, "%lld", (long long)in->ap_hits[13].pattern_id);
   char arg349[32];
   snprintf(arg349, sizeof arg349, "%d", in->ap_hits[13].hits);
   char arg350[32];
   snprintf(arg350, sizeof arg350, "%lld", (long long)in->ap_hits[14].pattern_id);
   char arg351[32];
   snprintf(arg351, sizeof arg351, "%d", in->ap_hits[14].hits);
   char arg352[32];
   snprintf(arg352, sizeof arg352, "%lld", (long long)in->ap_hits[15].pattern_id);
   char arg353[32];
   snprintf(arg353, sizeof arg353, "%d", in->ap_hits[15].hits);
   char arg354[32];
   snprintf(arg354, sizeof arg354, "%lld", (long long)in->ap_hits[16].pattern_id);
   char arg355[32];
   snprintf(arg355, sizeof arg355, "%d", in->ap_hits[16].hits);
   char arg356[32];
   snprintf(arg356, sizeof arg356, "%lld", (long long)in->ap_hits[17].pattern_id);
   char arg357[32];
   snprintf(arg357, sizeof arg357, "%d", in->ap_hits[17].hits);
   char arg358[32];
   snprintf(arg358, sizeof arg358, "%lld", (long long)in->ap_hits[18].pattern_id);
   char arg359[32];
   snprintf(arg359, sizeof arg359, "%d", in->ap_hits[18].hits);
   char arg360[32];
   snprintf(arg360, sizeof arg360, "%lld", (long long)in->ap_hits[19].pattern_id);
   char arg361[32];
   snprintf(arg361, sizeof arg361, "%d", in->ap_hits[19].hits);
   char arg362[32];
   snprintf(arg362, sizeof arg362, "%lld", (long long)in->ap_hits[20].pattern_id);
   char arg363[32];
   snprintf(arg363, sizeof arg363, "%d", in->ap_hits[20].hits);
   char arg364[32];
   snprintf(arg364, sizeof arg364, "%lld", (long long)in->ap_hits[21].pattern_id);
   char arg365[32];
   snprintf(arg365, sizeof arg365, "%d", in->ap_hits[21].hits);
   char arg366[32];
   snprintf(arg366, sizeof arg366, "%lld", (long long)in->ap_hits[22].pattern_id);
   char arg367[32];
   snprintf(arg367, sizeof arg367, "%d", in->ap_hits[22].hits);
   char arg368[32];
   snprintf(arg368, sizeof arg368, "%lld", (long long)in->ap_hits[23].pattern_id);
   char arg369[32];
   snprintf(arg369, sizeof arg369, "%d", in->ap_hits[23].hits);
   char arg370[32];
   snprintf(arg370, sizeof arg370, "%lld", (long long)in->ap_hits[24].pattern_id);
   char arg371[32];
   snprintf(arg371, sizeof arg371, "%d", in->ap_hits[24].hits);
   char arg372[32];
   snprintf(arg372, sizeof arg372, "%lld", (long long)in->ap_hits[25].pattern_id);
   char arg373[32];
   snprintf(arg373, sizeof arg373, "%d", in->ap_hits[25].hits);
   char arg374[32];
   snprintf(arg374, sizeof arg374, "%lld", (long long)in->ap_hits[26].pattern_id);
   char arg375[32];
   snprintf(arg375, sizeof arg375, "%d", in->ap_hits[26].hits);
   char arg376[32];
   snprintf(arg376, sizeof arg376, "%lld", (long long)in->ap_hits[27].pattern_id);
   char arg377[32];
   snprintf(arg377, sizeof arg377, "%d", in->ap_hits[27].hits);
   char arg378[32];
   snprintf(arg378, sizeof arg378, "%lld", (long long)in->ap_hits[28].pattern_id);
   char arg379[32];
   snprintf(arg379, sizeof arg379, "%d", in->ap_hits[28].hits);
   char arg380[32];
   snprintf(arg380, sizeof arg380, "%lld", (long long)in->ap_hits[29].pattern_id);
   char arg381[32];
   snprintf(arg381, sizeof arg381, "%d", in->ap_hits[29].hits);
   char arg382[32];
   snprintf(arg382, sizeof arg382, "%lld", (long long)in->ap_hits[30].pattern_id);
   char arg383[32];
   snprintf(arg383, sizeof arg383, "%d", in->ap_hits[30].hits);
   char arg384[32];
   snprintf(arg384, sizeof arg384, "%lld", (long long)in->ap_hits[31].pattern_id);
   char arg385[32];
   snprintf(arg385, sizeof arg385, "%d", in->ap_hits[31].hits);
   char arg386[32];
   snprintf(arg386, sizeof arg386, "%d", in->ap_hit_count);
   const char *fields[] = {sid, in->seen_paths[0], in->seen_paths[1], in->seen_paths[2], in->seen_paths[3], in->seen_paths[4], in->seen_paths[5], in->seen_paths[6], in->seen_paths[7], in->seen_paths[8], in->seen_paths[9], in->seen_paths[10], in->seen_paths[11], in->seen_paths[12], in->seen_paths[13], in->seen_paths[14], in->seen_paths[15], in->seen_paths[16], in->seen_paths[17], in->seen_paths[18], in->seen_paths[19], in->seen_paths[20], in->seen_paths[21], in->seen_paths[22], in->seen_paths[23], in->seen_paths[24], in->seen_paths[25], in->seen_paths[26], in->seen_paths[27], in->seen_paths[28], in->seen_paths[29], in->seen_paths[30], in->seen_paths[31], in->seen_paths[32], in->seen_paths[33], in->seen_paths[34], in->seen_paths[35], in->seen_paths[36], in->seen_paths[37], in->seen_paths[38], in->seen_paths[39], in->seen_paths[40], in->seen_paths[41], in->seen_paths[42], in->seen_paths[43], in->seen_paths[44], in->seen_paths[45], in->seen_paths[46], in->seen_paths[47], in->seen_paths[48], in->seen_paths[49], in->seen_paths[50], in->seen_paths[51], in->seen_paths[52], in->seen_paths[53], in->seen_paths[54], in->seen_paths[55], in->seen_paths[56], in->seen_paths[57], in->seen_paths[58], in->seen_paths[59], in->seen_paths[60], in->seen_paths[61], in->seen_paths[62], in->seen_paths[63], arg65, in->session_mode, in->guardrail_mode, arg68, arg69, arg70, in->worktrees[0].git_root, in->worktrees[0].worktree_path, in->worktrees[1].git_root, in->worktrees[1].worktree_path, in->worktrees[2].git_root, in->worktrees[2].worktree_path, in->worktrees[3].git_root, in->worktrees[3].worktree_path, in->worktrees[4].git_root, in->worktrees[4].worktree_path, in->worktrees[5].git_root, in->worktrees[5].worktree_path, in->worktrees[6].git_root, in->worktrees[6].worktree_path, in->worktrees[7].git_root, in->worktrees[7].worktree_path, in->worktrees[8].git_root, in->worktrees[8].worktree_path, in->worktrees[9].git_root, in->worktrees[9].worktree_path, in->worktrees[10].git_root, in->worktrees[10].worktree_path, in->worktrees[11].git_root, in->worktrees[11].worktree_path, in->worktrees[12].git_root, in->worktrees[12].worktree_path, in->worktrees[13].git_root, in->worktrees[13].worktree_path, in->worktrees[14].git_root, in->worktrees[14].worktree_path, in->worktrees[15].git_root, in->worktrees[15].worktree_path, arg103, arg104, arg105, arg106, arg107, arg108, arg109, in->tdd_mode, in->tdd_writes[0].stem, arg112, in->tdd_writes[1].stem, arg114, in->tdd_writes[2].stem, arg116, in->tdd_writes[3].stem, arg118, in->tdd_writes[4].stem, arg120, in->tdd_writes[5].stem, arg122, in->tdd_writes[6].stem, arg124, in->tdd_writes[7].stem, arg126, arg127, in->read_paths[0], in->read_paths[1], in->read_paths[2], in->read_paths[3], in->read_paths[4], in->read_paths[5], in->read_paths[6], in->read_paths[7], in->read_paths[8], in->read_paths[9], in->read_paths[10], in->read_paths[11], in->read_paths[12], in->read_paths[13], in->read_paths[14], in->read_paths[15], in->read_paths[16], in->read_paths[17], in->read_paths[18], in->read_paths[19], in->read_paths[20], in->read_paths[21], in->read_paths[22], in->read_paths[23], in->read_paths[24], in->read_paths[25], in->read_paths[26], in->read_paths[27], in->read_paths[28], in->read_paths[29], in->read_paths[30], in->read_paths[31], in->read_paths[32], in->read_paths[33], in->read_paths[34], in->read_paths[35], in->read_paths[36], in->read_paths[37], in->read_paths[38], in->read_paths[39], in->read_paths[40], in->read_paths[41], in->read_paths[42], in->read_paths[43], in->read_paths[44], in->read_paths[45], in->read_paths[46], in->read_paths[47], in->read_paths[48], in->read_paths[49], in->read_paths[50], in->read_paths[51], in->read_paths[52], in->read_paths[53], in->read_paths[54], in->read_paths[55], in->read_paths[56], in->read_paths[57], in->read_paths[58], in->read_paths[59], in->read_paths[60], in->read_paths[61], in->read_paths[62], in->read_paths[63], arg192, in->file_hashes[0].path, arg194, in->file_hashes[1].path, arg196, in->file_hashes[2].path, arg198, in->file_hashes[3].path, arg200, in->file_hashes[4].path, arg202, in->file_hashes[5].path, arg204, in->file_hashes[6].path, arg206, in->file_hashes[7].path, arg208, in->file_hashes[8].path, arg210, in->file_hashes[9].path, arg212, in->file_hashes[10].path, arg214, in->file_hashes[11].path, arg216, in->file_hashes[12].path, arg218, in->file_hashes[13].path, arg220, in->file_hashes[14].path, arg222, in->file_hashes[15].path, arg224, in->file_hashes[16].path, arg226, in->file_hashes[17].path, arg228, in->file_hashes[18].path, arg230, in->file_hashes[19].path, arg232, in->file_hashes[20].path, arg234, in->file_hashes[21].path, arg236, in->file_hashes[22].path, arg238, in->file_hashes[23].path, arg240, in->file_hashes[24].path, arg242, in->file_hashes[25].path, arg244, in->file_hashes[26].path, arg246, in->file_hashes[27].path, arg248, in->file_hashes[28].path, arg250, in->file_hashes[29].path, arg252, in->file_hashes[30].path, arg254, in->file_hashes[31].path, arg256, in->file_hashes[32].path, arg258, in->file_hashes[33].path, arg260, in->file_hashes[34].path, arg262, in->file_hashes[35].path, arg264, in->file_hashes[36].path, arg266, in->file_hashes[37].path, arg268, in->file_hashes[38].path, arg270, in->file_hashes[39].path, arg272, in->file_hashes[40].path, arg274, in->file_hashes[41].path, arg276, in->file_hashes[42].path, arg278, in->file_hashes[43].path, arg280, in->file_hashes[44].path, arg282, in->file_hashes[45].path, arg284, in->file_hashes[46].path, arg286, in->file_hashes[47].path, arg288, in->file_hashes[48].path, arg290, in->file_hashes[49].path, arg292, in->file_hashes[50].path, arg294, in->file_hashes[51].path, arg296, in->file_hashes[52].path, arg298, in->file_hashes[53].path, arg300, in->file_hashes[54].path, arg302, in->file_hashes[55].path, arg304, in->file_hashes[56].path, arg306, in->file_hashes[57].path, arg308, in->file_hashes[58].path, arg310, in->file_hashes[59].path, arg312, in->file_hashes[60].path, arg314, in->file_hashes[61].path, arg316, in->file_hashes[62].path, arg318, in->file_hashes[63].path, arg320, arg321, arg322, arg323, arg324, arg325, arg326, arg327, arg328, arg329, arg330, arg331, arg332, arg333, arg334, arg335, arg336, arg337, arg338, arg339, arg340, arg341, arg342, arg343, arg344, arg345, arg346, arg347, arg348, arg349, arg350, arg351, arg352, arg353, arg354, arg355, arg356, arg357, arg358, arg359, arg360, arg361, arg362, arg363, arg364, arg365, arg366, arg367, arg368, arg369, arg370, arg371, arg372, arg373, arg374, arg375, arg376, arg377, arg378, arg379, arg380, arg381, arg382, arg383, arg384, arg385, arg386};
   return write_result(call_stage(AIMEE_DB1_OP_SESSION_STATE_SAVE, fields, 387, NULL, NULL, 0, NULL));
}

int db1_session_state_delete(const char *sid)
{
   if (!sid || !sid[0])
      return -1;
   const char *fields[] = {sid};
   return write_result(call_stage(AIMEE_DB1_OP_SESSION_STATE_DELETE, fields, 1, NULL, NULL, 0, NULL));
}

int db1_session_state_exists(const char *sid)
{
   if (!sid || !sid[0])
      return -1;
   const char *fields[] = {sid};
   char slot0[32];
   char *const values[] = {slot0};
   const size_t caps[] = {sizeof slot0};
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STATE_EXISTS, fields, 1, values, caps, 1, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
      return -1;
   return (int)strtoll(slot0, NULL, 10);
}

int db1_session_state_list(db1_session_state_summary_t *out, int max)
{
   if (!out || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", max);
   const char *fields[] = {arg0};
   char **wire_values = malloc((size_t)max * 3u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 3u * sizeof *wire_caps);
   char (*wire_scratch)[32] = malloc((size_t)max * 1u * sizeof *wire_scratch);
   if (!wire_values || !wire_caps || !wire_scratch)
   {
      free(wire_values);
      free(wire_caps);
      free(wire_scratch);
      return -1;
   }
   memset(out, 0, (size_t)max * sizeof *out);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 3u + 0u] = out[wire_row].session_id;
      wire_caps[wire_row * 3u + 0u] = sizeof out[wire_row].session_id;
      wire_values[wire_row * 3u + 1u] = out[wire_row].updated_at;
      wire_caps[wire_row * 3u + 1u] = sizeof out[wire_row].updated_at;
      wire_values[wire_row * 3u + 2u] = wire_scratch[wire_row * 1u + 0u];
      wire_caps[wire_row * 3u + 2u] = sizeof wire_scratch[wire_row * 1u + 0u];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STATE_LIST, fields, 1, wire_values, wire_caps,
                           (uint32_t)(max * 3), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 3u != 0u)
   {
      free(wire_scratch);
      return -1;
   }
   int wire_rows = (int)(wire_filled / 3u);
   for (int wire_row = 0; wire_row < wire_rows; ++wire_row)
   {
      out[wire_row].hook_call_count = (int)strtol(wire_scratch[wire_row * 1u + 0u], NULL, 10);
   }
   free(wire_scratch);
   return wire_rows;
}

int db1_session_state_get_summary(const char *sid, db1_session_state_summary_t *out)
{
   if (!sid || !sid[0] || !out)
      return -1;
   const char *fields[] = {sid};
   char slot2[32];
   memset(out, 0, sizeof *out);
   char *const values[] = {out->session_id, out->updated_at, slot2};
   const size_t caps[] = {sizeof out->session_id, sizeof out->updated_at, sizeof slot2};
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STATE_GET_SUMMARY, fields, 1, values, caps, 3, NULL);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK)
   {
      return -1;
   }
   out->hook_call_count = (int)strtol(slot2, NULL, 10);
   return 0;
}

int db1_session_state_list_expired(int threshold_seconds, char (*out_ids)[DB1_SS_SID_LEN], int max)
{
   if (!out_ids || max <= 0)
      return -1;
   if (max > 256)
      max = 256;
   char arg0[32];
   snprintf(arg0, sizeof arg0, "%d", threshold_seconds);
   char arg1[32];
   snprintf(arg1, sizeof arg1, "%d", max);
   const char *fields[] = {arg0, arg1};
   char **wire_values = malloc((size_t)max * 1u * sizeof *wire_values);
   size_t *wire_caps = malloc((size_t)max * 1u * sizeof *wire_caps);
   if (!wire_values || !wire_caps)
   {
      free(wire_values);
      free(wire_caps);
      return -1;
   }
   memset(out_ids, 0, (size_t)max * sizeof *out_ids);
   for (int wire_row = 0; wire_row < max; ++wire_row)
   {
      wire_values[wire_row * 1u + 0u] = out_ids[wire_row];
      wire_caps[wire_row * 1u + 0u] = sizeof out_ids[wire_row];
   }
   uint32_t wire_filled = 0;
   int wire_status = call_stage(AIMEE_DB1_OP_SESSION_STATE_LIST_EXPIRED, fields, 2, wire_values, wire_caps,
                           (uint32_t)(max * 1), &wire_filled);
   free(wire_values);
   free(wire_caps);
   if (wire_status != (int)AIMEE_DB1_STATUS_OK || wire_filled % 1u != 0u)
   {
      return -1;
   }
   int wire_rows = (int)(wire_filled / 1u);
   return wire_rows;
}

/* clang-format on */
