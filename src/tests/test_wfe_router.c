/* test_wfe_router.c -- S1 request->workflow router pure core: catalog validation,
 * the deterministic prefilter (DEFER-trigger semantics + negation safety), the
 * decision/fallback table, and deterministic sampling. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "wfe_router.h"
#include "../server/router_advise.c"

static void add_wf(wfe_router_catalog_t *c, const char *id, int is_default, int read_only)
{
   wfe_router_wf_t *w = &c->wf[c->n++];
   memset(w, 0, sizeof *w);
   snprintf(w->id, sizeof w->id, "%s", id);
   w->is_default = is_default;
   w->read_only = read_only;
}

/* converse + research(default,ro) + managed-change + hotfix + build */
static void mkcat(wfe_router_catalog_t *c)
{
   c->n = 0;
   add_wf(c, "converse", 0, 1);
   add_wf(c, "research", 1, 1); /* the read-only default */
   add_wf(c, "managed-change", 0, 0);
   add_wf(c, "hotfix", 0, 0);
   add_wf(c, "build", 0, 0);
}

static const char *route(const wfe_router_catalog_t *c, const char *msg, const char *classifier)
{
   static wfe_route_decision_t d;
   wfe_router_decide(msg, c, classifier, &d);
   return d.workflow_id;
}

static char observed_query[256];
static int routed, bound;
int wfe_router_catalog_load(wfe_router_catalog_t *out, char *error, size_t cap)
{
   (void)error;
   (void)cap;
   mkcat(out);
   routed++;
   return 0;
}
const char *ingress_preinject_turn_id(void)
{
   return "visible-turn";
}
const char *ingress_preinject_session_id(void)
{
   return "visible-session";
}
const char *ie_event_type_name(ie_event_type_t type)
{
   (void)type;
   return "guardrail_decision";
}
int db1_interaction_event_record(const char *session, const char *type, const char *actor,
                                 const char *payload, const char *outcome)
{
   (void)type;
   (void)actor;
   (void)payload;
   (void)outcome;
   assert(!strcmp(session, "visible-turn"));
   return 0;
}
wfe_enforce_stage_t wfe_enforce_stage_parse(const char *value)
{
   (void)value;
   return WFE_ENFORCE_OFF;
}
const char *wfe_enforce_stage_name(wfe_enforce_stage_t stage)
{
   (void)stage;
   return "off";
}
int wfe_bind_interactive(const char *session, const char *query, const char *repo)
{
   (void)repo;
   assert(!strcmp(session, "visible-session"));
   snprintf(observed_query, sizeof observed_query, "%s", query);
   bound++;
   return 0;
}
static void test_router_uses_only_visible_ir(void)
{
   aimee_block_t blocks[3] = {{.type = AIMEE_BLK_TEXT, .text = "visible task"},
                              {.type = AIMEE_BLK_TEXT, .text = "hidden secret"},
                              {.type = AIMEE_BLK_TEXT, .text = "restricted secret"}};
   for (int i = 0; i < 3; i++)
      aimee_ir_block_set_context(&blocks[i], AIMEE_CTX_ORIGIN_USER, AIMEE_CTX_AUTH_TASK_INSTRUCTION,
                                 AIMEE_CTX_TRUST_UNVERIFIED,
                                 i == 2 ? AIMEE_CTX_SENS_RESTRICTED : AIMEE_CTX_SENS_INTERNAL,
                                 i != 1, NULL, NULL, 0);
   aimee_message_t message = {.role = "user", .blocks = blocks, .n_blocks = 3};
   aimee_request_t request = {.messages = &message, .n_messages = 1};
   aimee_block_t before[3];
   memcpy(before, blocks, sizeof before);
   router_advise_ir(&request);
   assert(routed == 1 && bound == 1 && !strcmp(observed_query, "visible task"));
   assert(!memcmp(before, blocks, sizeof before));
   message.blocks = &blocks[1];
   message.n_blocks = 2;
   router_advise_ir(&request);
   assert(routed == 1 && bound == 1);
   assert(!memcmp(before, blocks, sizeof before));
}

int main(void)
{
   test_router_uses_only_visible_ir();
   printf("wfe-router: ");
   char err[128];
   wfe_router_catalog_t c;
   mkcat(&c);

   /* --- id validity (charset + length; used for JSON-safe logging + routing) --- */
   assert(wfe_router_id_valid("managed-change") && wfe_router_id_valid("build_2"));
   assert(!wfe_router_id_valid("") && !wfe_router_id_valid(NULL));
   assert(!wfe_router_id_valid("../evil") && !wfe_router_id_valid("has space"));
   assert(!wfe_router_id_valid("quote\"inject") && !wfe_router_id_valid("path/sep"));

   /* --- catalog validation --- */
   assert(wfe_router_catalog_validate(&c, err, sizeof err) == 0);
   {
      wfe_router_catalog_t badid = c; /* an invalid id in the catalog is rejected */
      snprintf(badid.wf[3].id, sizeof badid.wf[3].id, "bad\"id");
      assert(wfe_router_catalog_validate(&badid, err, sizeof err) != 0);
   }

   wfe_router_catalog_t bad = c; /* two defaults */
   bad.wf[2].is_default = 1;
   assert(wfe_router_catalog_validate(&bad, err, sizeof err) != 0);

   bad = c; /* zero defaults */
   bad.wf[1].is_default = 0;
   assert(wfe_router_catalog_validate(&bad, err, sizeof err) != 0);

   bad = c; /* default not read-only */
   bad.wf[1].read_only = 0;
   assert(wfe_router_catalog_validate(&bad, err, sizeof err) != 0);
   assert(strstr(err, "read-only") != NULL);

   bad = c; /* duplicate id */
   snprintf(bad.wf[4].id, sizeof bad.wf[4].id, "hotfix");
   assert(wfe_router_catalog_validate(&bad, err, sizeof err) != 0);

   bad = c; /* no converse lane */
   snprintf(bad.wf[0].id, sizeof bad.wf[0].id, "chitchat");
   assert(wfe_router_catalog_validate(&bad, err, sizeof err) != 0);

   /* --- prefilter --- */
   char mid[64], reason[96];
   assert(wfe_router_prefilter("", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_CONVERSE);
   assert(wfe_router_prefilter("   ", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_CONVERSE);
   assert(wfe_router_prefilter("just chat please", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_CONVERSE);
   /* pure question, no change verb, no code -> converse */
   assert(wfe_router_prefilter("what does this function do?", &c, mid, sizeof mid, reason,
                               sizeof reason) == WFE_PREFILTER_CONVERSE);
   /* explicit valid use -> named */
   assert(wfe_router_prefilter("use managed-change", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_NAMED);
   assert(strcmp(mid, "managed-change") == 0);
   /* explicit unknown name -> defer (never fuzzy) */
   assert(wfe_router_prefilter("use bogus-wf", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_DEFER);
   /* change verb -> defer (NOT auto route-to-change) */
   assert(wfe_router_prefilter("implement a logout button", &c, mid, sizeof mid, reason,
                               sizeof reason) == WFE_PREFILTER_DEFER);
   /* NEGATION/QUESTION SAFETY: a question containing a change verb defers, it
    * does NOT route to a write workflow. */
   assert(wfe_router_prefilter("how do I change my password?", &c, mid, sizeof mid, reason,
                               sizeof reason) == WFE_PREFILTER_DEFER);
   /* code/path token but no verb -> defer */
   assert(wfe_router_prefilter("look at src/foo.c", &c, mid, sizeof mid, reason, sizeof reason) ==
          WFE_PREFILTER_DEFER);

   /* --- decision + fallback table --- */
   assert(strcmp(route(&c, "use build", NULL), "build") == 0);      /* named */
   assert(strcmp(route(&c, "hello there", NULL), "converse") == 0); /* converse */
   assert(strcmp(route(&c, "fix the bug", "managed-change"), "managed-change") ==
          0); /* classifier */
   assert(strcmp(route(&c, "fix the bug", NULL), "research") ==
          0); /* defer, no classifier -> default */
   assert(strcmp(route(&c, "fix the bug", "not-a-workflow"), "research") ==
          0); /* out-of-catalog -> default */
   /* the router never emits an id outside the catalog */
   wfe_route_decision_t d;
   wfe_router_decide("refactor everything", &c, "../evil", &d);
   assert(wfe_router_find(&c, d.workflow_id) != NULL);

   /* --- classifier prompt + response parsing --- */
   char sys[1024];
   wfe_router_classify_prompt(&c, sys, sizeof sys);
   assert(strstr(sys, "managed-change") && strstr(sys, "research"));

   char cid[64];
   assert(wfe_router_parse_classification("managed-change", &c, cid, sizeof cid) == 0 &&
          strcmp(cid, "managed-change") == 0);
   assert(wfe_router_parse_classification("The workflow is managed-change.", &c, cid, sizeof cid) ==
              0 &&
          strcmp(cid, "managed-change") == 0);
   assert(wfe_router_parse_classification("```build```", &c, cid, sizeof cid) == 0 &&
          strcmp(cid, "build") == 0);
   assert(wfe_router_parse_classification("none of these apply", &c, cid, sizeof cid) != 0);
   /* a substring that isn't a bounded token must NOT match (e.g. 'rebuild' != 'build') */
   assert(wfe_router_parse_classification("rebuild the index", &c, cid, sizeof cid) != 0);
   /* reasoning-style reply: earlier ids are mentioned but the ANSWER is last */
   assert(wfe_router_parse_classification("Not converse or research; answer: managed-change", &c,
                                          cid, sizeof cid) == 0 &&
          strcmp(cid, "managed-change") == 0);

   /* --- advisory payload: structural features only, no raw text --- */
   wfe_route_decision_t pd;
   wfe_router_decide("use build", &c, NULL, &pd);
   char pj[512];
   wfe_router_advisory_payload(&pd, WFE_PREFILTER_NAMED, 0, -1.0, pj, sizeof pj);
   assert(strstr(pj, "\"workflow_id\":\"build\"") && strstr(pj, "\"source\":\"prefilter\""));
   assert(strstr(pj, "\"user_provided_name\":true"));
   assert(strstr(pj, "classifier_ms") == NULL); /* not run -> omitted */
   wfe_router_advisory_payload(&pd, WFE_PREFILTER_DEFER, 1, 42.5, pj, sizeof pj);
   assert(strstr(pj, "\"classifier_ms\":42.5") && strstr(pj, "\"sampled\":true"));

   /* --- deterministic sampling --- */
   assert(wfe_router_should_sample("s", 0, 1) == 1); /* 1-in-1 always */
   assert(wfe_router_should_sample("sess", 7, 10) ==
          wfe_router_should_sample("sess", 7, 10)); /* stable */
   int hits = 0;
   for (int i = 0; i < 1000; i++)
      hits += wfe_router_should_sample("sess", i, 10);
   assert(hits > 40 && hits < 200); /* ~1/10, loosely */

   printf("ok\n");
   return 0;
}
