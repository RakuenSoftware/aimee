#include "original_input_plan.h"
#include "aimee_sha256.h"
#include "cJSON.h"
#include "db1_module_api.h"
#include "transport.h"
#include "util.h"
#include <aimee/ir/aimee_ir.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
static db1_family_t plan_family = {"db1.input-plan",         "sessions", AIMEE_DB1_EVENT_SESSIONS,
                                   AIMEE_DB1_STAGE_SESSIONS, 30000,      0};
__attribute__((used, externally_visible, retain))
const unsigned aimee_original_input_safe_plan_client_contract_v1 = 1;
static int positive(const char *s, uint64_t *out)
{
   if (!s || !*s || (s[0] == '0' && s[1]))
      return 0;
   uint64_t n = 0;
   for (; *s; s++)
   {
      if (*s < '0' || *s > '9' || n > ((uint64_t)INT64_MAX - (unsigned)(*s - '0')) / 10)
         return 0;
      n = n * 10 + (unsigned)(*s - '0');
   }
   *out = n;
   return n != 0;
}
static int digest(const char *s)
{
   if (!s || strlen(s) != 64)
      return 0;
   for (; *s; s++)
      if (!(*s >= '0' && *s <= '9') && !(*s >= 'a' && *s <= 'f'))
         return 0;
   return 1;
}
static const char *string(const cJSON *o, const char *key)
{
   return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, key));
}
static int keys(const cJSON *o, const char *const *allowed, size_t n)
{
   if (!cJSON_IsObject(o) || cJSON_GetArraySize(o) != (int)n)
      return 0;
   for (const cJSON *c = o->child; c; c = c->next)
   {
      size_t i = 0;
      for (; i < n; i++)
         if (!strcmp(c->string, allowed[i]))
            break;
      if (i == n)
         return 0;
   }
   return 1;
}
static int version(const cJSON *o)
{
   const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, "schema_version");
   return cJSON_IsNumber(v) && v->valuedouble == 1;
}
static int copy(char *to, size_t cap, const char *from)
{
   if (!from || strlen(from) >= cap)
      return 0;
   memcpy(to, from, strlen(from) + 1);
   return 1;
}
static int plan_parse(const cJSON *o, db1_original_input_plan_receipt_t *out)
{
   static const char *const fields[] = {
       "schema_version", "plan_id",          "session_id",    "operation_uuid", "actor",
       "receiver_tag",   "safe_text_sha256", "plan_sha256",   "opaque_plan",    "state",
       "begin_sequence", "input_sequence",   "original_turn", "delivery_id",    "terminal_result"};
   db1_original_input_plan_receipt_t p = {0};
   const char *id = string(o, "plan_id"), *session = string(o, "session_id"),
              *op = string(o, "operation_uuid"), *actor = string(o, "actor"),
              *receiver = string(o, "receiver_tag"), *text = string(o, "safe_text_sha256"),
              *hash = string(o, "plan_sha256"), *opaque = string(o, "opaque_plan"),
              *state = string(o, "state"), *begin = string(o, "begin_sequence"),
              *input = string(o, "input_sequence"), *turn = string(o, "original_turn"),
              *delivery = string(o, "delivery_id"), *result = string(o, "terminal_result");
   char actual[65];
   uint64_t a = 0, b = 0, d = 0;
   if (!keys(o, fields, 15) || !version(o) || !positive(id, &p.plan_id) || !session || !*session ||
       !actor || !*actor || !receiver || !*receiver || !op || strlen(op) != 36 || !digest(text) ||
       !digest(hash) || !opaque || !*opaque || strlen(opaque) > 16384 ||
       aimee_sha256_hex(opaque, strlen(opaque), actual) || strcmp(actual, hash) || !state ||
       !begin || !input || !turn || !delivery || !result || strlen(result) > 32768)
      return -1;
   if (!strcmp(state, "reserved"))
   {
      if (*begin || *input || *turn || *delivery || *result)
         return -1;
      p.state = DB1_INPUT_PLAN_RESERVED;
   }
   else if (!strcmp(state, "terminal"))
   {
      if (*begin || *input || *turn || *delivery || !*result)
         return -1;
      cJSON *proof = aimee_ir_json_parse_frame(result, strlen(result));
      if (!proof)
         return -1;
      cJSON_Delete(proof);
      p.state = DB1_INPUT_PLAN_TERMINAL;
   }
   else if (!strcmp(state, "committed"))
   {
      if (!positive(begin, &a) || !positive(input, &b) || b != a + 1 || !*turn ||
          strlen(turn) > 1024 || !positive(delivery, &d) || *result)
         return -1;
      p.state = DB1_INPUT_PLAN_COMMITTED;
   }
   else
      return -1;
   if (!copy(p.session, sizeof p.session, session) ||
       !copy(p.operation_uuid, sizeof p.operation_uuid, op) ||
       !copy(p.actor, sizeof p.actor, actor) ||
       !copy(p.receiver_tag, sizeof p.receiver_tag, receiver) ||
       !copy(p.safe_text_sha256, sizeof p.safe_text_sha256, text) ||
       !copy(p.plan_sha256, sizeof p.plan_sha256, hash))
      return -1;
   p.contract_version = 1;
   *out = p;
   return 0;
}
static int read_one(uint32_t op, const char *const *args, uint32_t n, char **out)
{
   char *wire = calloc(1, 524288);
   if (!wire)
      return -1;
   char *values[] = {wire};
   const size_t caps[] = {524288};
   uint32_t filled = 0;
   int status = db1_call(&plan_family, op, args, n, values, caps, 1, &filled);
   if (status != AIMEE_DB1_STATUS_OK || filled != 1 || !wire[0])
   {
      free(wire);
      return status == AIMEE_DB1_STATUS_CONFLICT ? DB1_INPUT_PLAN_CONFLICT : -1;
   }
   *out = wire;
   return 0;
}
__attribute__((used, externally_visible, retain)) void
db1_original_input_probe_destroy(db1_original_input_probe_t *out)
{
   if (out)
   {
      free(out->opaque_plan);
      free(out->terminal_result);
      memset(out, 0, sizeof *out);
   }
}
__attribute__((used, externally_visible, retain)) int
db1_session_original_input_probe(const char *session, const char *operation, const char *actor,
                                 db1_original_input_probe_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!session || !operation || !actor)
      return -1;
   const char *args[] = {session, operation, actor};
   char *wire = NULL;
   int rc = read_one(AIMEE_DB1_OP_SESSION_ORIGINAL_INPUT_PROBE, args, 3, &wire);
   if (rc)
      return rc;
   cJSON *o = aimee_ir_json_parse_frame(wire, strlen(wire));
   free(wire);
   db1_original_input_probe_t p = {0};
   static const char *const fields[] = {"schema_version", "presence", "session_id",
                                        "operation_uuid", "actor",    "canonical",
                                        "delivery_id",    "plan"};
   const char *presence = string(o, "presence"), *sid = string(o, "session_id"),
              *op = string(o, "operation_uuid"), *who = string(o, "actor"),
              *did = string(o, "delivery_id");
   const cJSON *canonical = cJSON_GetObjectItemCaseSensitive(o, "canonical"),
               *plan = cJSON_GetObjectItemCaseSensitive(o, "plan");
   if (!keys(o, fields, 8) || !version(o) || !presence || !sid || strcmp(sid, session) || !op ||
       strcmp(op, operation) || !who || strcmp(who, actor) || !did)
      goto bad;
   if (*did && !positive(did, &p.delivery_id))
      goto bad;
   if (!cJSON_IsNull(canonical))
   {
      static const char *const names[] = {"begin_sequence", "input_sequence", "original_turn"};
      const char *a = string(canonical, "begin_sequence"), *b = string(canonical, "input_sequence"),
                 *turn = string(canonical, "original_turn");
      if (!keys(canonical, names, 3) || !positive(a, &p.canonical.begin_sequence) ||
          !positive(b, &p.canonical.input_sequence) ||
          p.canonical.input_sequence != p.canonical.begin_sequence + 1 || !turn || !*turn ||
          !copy(p.canonical.original_turn, sizeof p.canonical.original_turn, turn))
         goto bad;
      p.canonical.contract_version = 1;
      p.canonical.outcome = DB1_ORIGINAL_INPUT_REPLAY;
   }
   if (!cJSON_IsNull(plan))
   {
      if (plan_parse(plan, &p.plan) || strcmp(p.plan.session, session) ||
          strcmp(p.plan.operation_uuid, operation) || strcmp(p.plan.actor, actor))
         goto bad;
      p.opaque_plan = strdup(string(plan, "opaque_plan"));
      p.terminal_result = strdup(string(plan, "terminal_result"));
      if (!p.opaque_plan || !p.terminal_result)
         goto bad;
      p.opaque_plan_len = strlen(p.opaque_plan);
      p.terminal_result_len = strlen(p.terminal_result);
   }
   if (!strcmp(presence, "owned_absent"))
   {
      if (p.canonical.contract_version || p.delivery_id || p.plan.contract_version)
         goto bad;
      p.presence = DB1_INPUT_PROBE_OWNED_ABSENT;
   }
   else if (!strcmp(presence, "legacy"))
   {
      if (!p.canonical.contract_version || p.delivery_id || p.plan.contract_version)
         goto bad;
      p.presence = DB1_INPUT_PROBE_LEGACY;
   }
   else if (!strcmp(presence, "delivery"))
   {
      if (!p.canonical.contract_version || !p.delivery_id || p.plan.contract_version)
         goto bad;
      p.presence = DB1_INPUT_PROBE_DELIVERY;
   }
   else if (!strcmp(presence, "reserved"))
   {
      if (p.canonical.contract_version || p.delivery_id || p.plan.state != DB1_INPUT_PLAN_RESERVED)
         goto bad;
      p.presence = DB1_INPUT_PROBE_RESERVED;
   }
   else if (!strcmp(presence, "terminal"))
   {
      if (p.canonical.contract_version || p.delivery_id || p.plan.state != DB1_INPUT_PLAN_TERMINAL)
         goto bad;
      p.presence = DB1_INPUT_PROBE_TERMINAL;
   }
   else if (!strcmp(presence, "committed"))
   {
      if (!p.canonical.contract_version || !p.delivery_id ||
          p.plan.state != DB1_INPUT_PLAN_COMMITTED ||
          strcmp(string(plan, "begin_sequence"), string(canonical, "begin_sequence")) ||
          strcmp(string(plan, "input_sequence"), string(canonical, "input_sequence")) ||
          strcmp(string(plan, "original_turn"), p.canonical.original_turn) ||
          strcmp(string(plan, "delivery_id"), did))
         goto bad;
      p.presence = DB1_INPUT_PROBE_COMMITTED;
   }
   else
      goto bad;
   p.contract_version = 1;
   *out = p;
   cJSON_Delete(o);
   return 0;
bad:
   db1_original_input_probe_destroy(&p);
   cJSON_Delete(o);
   return -1;
}
__attribute__((used, externally_visible, retain)) int db1_session_original_input_plan_reserve(
    const char *session, const char *operation, const char *actor, const char *receiver,
    const char *text_sha, const char *plan_sha, const char *opaque_plan,
    db1_original_input_plan_receipt_t *out)
{
   if (!out)
      return -1;
   memset(out, 0, sizeof *out);
   if (!session || !operation || !actor || !receiver || !text_sha || !plan_sha || !opaque_plan)
      return -1;
   const char *args[] = {session, operation, actor, receiver, text_sha, plan_sha, opaque_plan};
   char *wire = NULL;
   int rc = read_one(AIMEE_DB1_OP_SESSION_ORIGINAL_INPUT_PLAN_RESERVE, args, 7, &wire);
   if (rc)
      return rc;
   cJSON *o = aimee_ir_json_parse_frame(wire, strlen(wire));
   free(wire);
   db1_original_input_plan_receipt_t p = {0};
   int valid = !plan_parse(o, &p) && p.state == DB1_INPUT_PLAN_RESERVED &&
               !strcmp(p.session, session) && !strcmp(p.operation_uuid, operation) &&
               !strcmp(p.actor, actor) && !strcmp(p.receiver_tag, receiver) &&
               !strcmp(p.safe_text_sha256, text_sha) && !strcmp(p.plan_sha256, plan_sha) &&
               !strcmp(string(o, "opaque_plan"), opaque_plan);
   cJSON_Delete(o);
   if (!valid)
      return -1;
   *out = p;
   return 0;
}
__attribute__((used, externally_visible, retain)) int
db1_session_original_input_begin_planned_delivery(const char *session, const char *operation,
                                                  const char *turn, const char *actor,
                                                  const char *batch, const char *descriptor,
                                                  const char *plan_id, const char *plan_sha,
                                                  db1_original_input_receipt_t *out,
                                                  uint64_t *delivery_id)
{
   if (!out || !delivery_id)
      return -1;
   memset(out, 0, sizeof *out);
   *delivery_id = 0;
   if (!session || !operation || !turn || !actor || !batch || !descriptor || !plan_id || !plan_sha)
      return -1;
   const char *args[] = {session, operation, turn, actor, batch, descriptor, plan_id, plan_sha};
   char first[32] = {0}, last[32] = {0}, original[1025] = {0}, version_[2] = {0}, outcome[8] = {0},
        id[32] = {0};
   char *values[] = {first, last, original, version_, outcome, id};
   const size_t caps[] = {sizeof first,    sizeof last,    sizeof original,
                          sizeof version_, sizeof outcome, sizeof id};
   uint32_t filled = 0;
   int status = db1_call(&plan_family, AIMEE_DB1_OP_SESSION_ORIGINAL_INPUT_BEGIN_PLANNED_DELIVERY,
                         args, 8, values, caps, 6, &filled);
   db1_original_input_receipt_t p = {0};
   uint64_t di = 0;
   if (status != AIMEE_DB1_STATUS_OK)
      return status == AIMEE_DB1_STATUS_CONFLICT ? DB1_INPUT_PLAN_CONFLICT : -1;
   if (filled != 6 || strcmp(version_, "1") || !positive(first, &p.begin_sequence) ||
       !positive(last, &p.input_sequence) || p.input_sequence != p.begin_sequence + 1 ||
       !positive(id, &di) || !original[0])
      return -1;
   int created = !strcmp(outcome, "created"), replay = !strcmp(outcome, "replay");
   if ((!created && !replay) || (created && strcmp(original, turn)))
      return -1;
   if (!text_is_valid_utf8(original))
      return -1;
   memcpy(p.original_turn, original, strlen(original) + 1);
   p.contract_version = 1;
   p.outcome = created ? DB1_ORIGINAL_INPUT_CREATED : DB1_ORIGINAL_INPUT_REPLAY;
   *out = p;
   *delivery_id = di;
   return created ? 0 : 1;
}
__attribute__((used, externally_visible, retain)) int
db1_session_original_input_plan_terminal(const char *id, const char *session, const char *operation,
                                         const char *actor, const char *receiver, const char *hash,
                                         const char *result)
{
   if (!id || !session || !operation || !actor || !receiver || !hash || !result)
      return -1;
   const char *args[] = {id, session, operation, actor, receiver, hash, result};
   char version_[2] = {0};
   char *values[] = {version_};
   const size_t caps[] = {sizeof version_};
   uint32_t filled = 0;
   int status = db1_call(&plan_family, AIMEE_DB1_OP_SESSION_ORIGINAL_INPUT_PLAN_TERMINAL, args, 7,
                         values, caps, 1, &filled);
   if (status == AIMEE_DB1_STATUS_CONFLICT)
      return DB1_INPUT_PLAN_CONFLICT;
   return status == AIMEE_DB1_STATUS_OK && filled == 1 && !strcmp(version_, "1") ? 0 : -1;
}
