/* wfe_def.c -- workflow definition: block catalog + YAML->struct parser. */
#include "wfe_def.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "yaml.h"

/* ---- Block catalog: the frozen typed-I/O vocabulary (W1). ---- */
static const struct
{
   wfe_block_type_t t;
   const char *name;
   wfe_artifact_type_t output;
   int requires_input;
   wfe_artifact_type_t accepts[8]; /* WFE_ART_NONE-terminated */
} CATALOG[] = {
    {WFE_BLK_AUTHOR_PROPOSAL, "author.proposal", WFE_ART_PROPOSAL, 0, {WFE_ART_NONE}},
    {WFE_BLK_AUTHOR_PLAN, "author.plan", WFE_ART_PLAN, 1, {WFE_ART_PROPOSAL, WFE_ART_NONE}},
    /* implement also accepts an INTENT directly (S0): a single-packet `hotfix`
     * feeds understand -> implement without a split step. */
    {WFE_BLK_IMPLEMENT,
     "implement",
     WFE_ART_BRANCH,
     1,
     {WFE_ART_PLAN, WFE_ART_INTENT, WFE_ART_NONE}},
    /* document the effort: a delegate writes docs onto the branch (consumes a
     * branch, produces the documented branch) -> composes between implement and
     * freeze, or anywhere a branch is in hand. */
    {WFE_BLK_DOCUMENT, "document", WFE_ART_BRANCH, 1, {WFE_ART_BRANCH, WFE_ART_NONE}},
    {WFE_BLK_FREEZE, "freeze", WFE_ART_FROZEN_DIFF, 1, {WFE_ART_BRANCH, WFE_ART_NONE}},
    {WFE_BLK_GATE_ROUNDTABLE,
     "gate.roundtable",
     WFE_ART_VERDICT,
     1,
     {WFE_ART_PROPOSAL, WFE_ART_PLAN, WFE_ART_FROZEN_DIFF, WFE_ART_NONE}},
    {WFE_BLK_GATE_HUMAN,
     "gate.human",
     WFE_ART_APPROVAL,
     1,
     {WFE_ART_PROPOSAL, WFE_ART_PLAN, WFE_ART_BRANCH, WFE_ART_FROZEN_DIFF, WFE_ART_PR,
      WFE_ART_NONE}},
    {WFE_BLK_PR_OPEN,
     "pr.open",
     WFE_ART_PR,
     1,
     {WFE_ART_PROPOSAL, WFE_ART_FROZEN_DIFF, WFE_ART_NONE}},
    {WFE_BLK_MERGE, "merge", WFE_ART_NONE, 1, {WFE_ART_PR, WFE_ART_NONE}},
    /* safety gates: poll the PR's CI / mergeability (pr -> verdict). */
    {WFE_BLK_GATE_CI, "gate.ci", WFE_ART_VERDICT, 1, {WFE_ART_PR, WFE_ART_NONE}},
    {WFE_BLK_CHECK_MERGEABLE, "check.mergeable", WFE_ART_VERDICT, 1, {WFE_ART_PR, WFE_ART_NONE}},
    /* primary-as-manager (S0): the interactive manager loop.
     * understand (source, with the user) -> split -> implement -> review ->
     * gate.roundtable -> gate.deliver. review consumes the delegate branch and
     * emits a verdict; gate.deliver is a terminal enforcement gate. */
    {WFE_BLK_UNDERSTAND, "understand", WFE_ART_INTENT, 0, {WFE_ART_NONE}},
    {WFE_BLK_SPLIT, "split", WFE_ART_PLAN, 1, {WFE_ART_INTENT, WFE_ART_NONE}},
    {WFE_BLK_REVIEW,
     "review",
     WFE_ART_VERDICT,
     1,
     {WFE_ART_FROZEN_DIFF, WFE_ART_BRANCH, WFE_ART_NONE}},
    {WFE_BLK_GATE_DELIVER,
     "gate.deliver",
     WFE_ART_NONE,
     1,
     {WFE_ART_VERDICT, WFE_ART_APPROVAL, WFE_ART_NONE}},
};
static const int CATALOG_N = (int)(sizeof(CATALOG) / sizeof(CATALOG[0]));

static const char *ARTIFACT_NAMES[WFE_ART__COUNT] = {
    "none", "proposal", "plan", "branch", "frozen_diff", "pr", "verdict", "approval", "intent"};

const char *wfe_artifact_name(wfe_artifact_type_t t)
{
   if (t < 0 || t >= WFE_ART__COUNT)
      return "?";
   return ARTIFACT_NAMES[t];
}

static int catalog_index(wfe_block_type_t t)
{
   for (int i = 0; i < CATALOG_N; i++)
      if (CATALOG[i].t == t)
         return i;
   return -1;
}

const char *wfe_block_name(wfe_block_type_t t)
{
   int i = catalog_index(t);
   return i >= 0 ? CATALOG[i].name : "unknown";
}

wfe_block_type_t wfe_block_from_name(const char *name)
{
   if (!name)
      return WFE_BLK_UNKNOWN;
   for (int i = 0; i < CATALOG_N; i++)
      if (strcmp(CATALOG[i].name, name) == 0)
         return CATALOG[i].t;
   /* fall through to the config-defined registry (consults already-loaded
    * state only -- callers ensure() before parse/validate, never here, to
    * avoid reentrancy during registry load). */
   if (wfe_custom_lookup(name))
      return WFE_BLK_CUSTOM;
   return WFE_BLK_UNKNOWN;
}

wfe_artifact_type_t wfe_block_output(wfe_block_type_t t)
{
   int i = catalog_index(t);
   return i >= 0 ? CATALOG[i].output : WFE_ART_NONE;
}

int wfe_block_requires_input(wfe_block_type_t t)
{
   int i = catalog_index(t);
   return i >= 0 ? CATALOG[i].requires_input : 0;
}

int wfe_block_accepts_input(wfe_block_type_t t, wfe_artifact_type_t in)
{
   int i = catalog_index(t);
   if (i < 0)
      return 0;
   for (int j = 0; CATALOG[i].accepts[j] != WFE_ART_NONE && j < 8; j++)
      if (CATALOG[i].accepts[j] == in)
         return 1;
   return 0;
}

static void copy_str(char *dst, size_t cap, const char *src)
{
   if (!src)
   {
      dst[0] = '\0';
      return;
   }
   snprintf(dst, cap, "%s", src);
}

static const char *obj_str(const cJSON *obj, const char *key)
{
   const cJSON *it = cJSON_GetObjectItemCaseSensitive(obj, key);
   return (it && cJSON_IsString(it)) ? it->valuestring : NULL;
}

/* Parse a binding value "<producer_id>[.<output>]" into producer/output. */
static void parse_ref(const char *ref, char *prod, size_t prodcap, char *out, size_t outcap)
{
   const char *dot = ref ? strrchr(ref, '.') : NULL;
   if (dot)
   {
      size_t plen = (size_t)(dot - ref);
      if (plen >= prodcap)
         plen = prodcap - 1;
      memcpy(prod, ref, plen);
      prod[plen] = '\0';
      copy_str(out, outcap, dot + 1);
   }
   else
   {
      copy_str(prod, prodcap, ref);
      copy_str(out, outcap, "out");
   }
}

static int parse_node(wfe_node_t *n, const cJSON *jn, char *err, size_t errlen)
{
   const char *id = obj_str(jn, "id");
   if (!id || !*id)
   {
      snprintf(err, errlen, "node missing 'id'");
      return -1;
   }
   copy_str(n->id, sizeof n->id, id);

   const char *blk = obj_str(jn, "block");
   n->block = wfe_block_from_name(blk);
   if (n->block == WFE_BLK_UNKNOWN)
   {
      snprintf(err, errlen, "node '%s': unknown block '%s'", n->id, blk ? blk : "");
      return -1;
   }
   if (n->block == WFE_BLK_CUSTOM)
      copy_str(n->custom_name, sizeof n->custom_name, blk);

   n->params = cJSON_GetObjectItemCaseSensitive(jn, "params");
   copy_str(n->next, sizeof n->next, obj_str(jn, "next"));
   copy_str(n->on_pass, sizeof n->on_pass, obj_str(jn, "on_pass"));
   copy_str(n->on_fail, sizeof n->on_fail, obj_str(jn, "on_fail"));

   const cJSON *ins = cJSON_GetObjectItemCaseSensitive(jn, "in");
   n->n_ins = 0;
   if (ins && cJSON_IsObject(ins))
   {
      const cJSON *b = NULL;
      cJSON_ArrayForEach(b, ins)
      {
         if (n->n_ins >= WFE_MAX_INS)
         {
            snprintf(err, errlen, "node '%s': too many inputs", n->id);
            return -1;
         }
         if (!cJSON_IsString(b))
         {
            snprintf(err, errlen, "node '%s': input '%s' must be a string ref", n->id,
                     b->string ? b->string : "?");
            return -1;
         }
         wfe_binding_t *bind = &n->ins[n->n_ins++];
         copy_str(bind->input_name, sizeof bind->input_name, b->string);
         parse_ref(b->valuestring, bind->producer_id, sizeof bind->producer_id, bind->output_name,
                   sizeof bind->output_name);
      }
   }
   return 0;
}

wfe_def_t *wfe_def_parse(const char *yaml_text, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   /* make config-defined block names resolvable before we parse node blocks */
   char cerr[256];
   if (wfe_custom_registry_ensure(cerr, sizeof cerr) != 0)
   {
      snprintf(err, errlen, "custom block registry: %s", cerr);
      return NULL;
   }
   cJSON *root = yaml_parse(yaml_text ? yaml_text : "");
   if (!root || !cJSON_IsObject(root))
   {
      snprintf(err, errlen, "could not parse workflow YAML");
      if (root)
         cJSON_Delete(root);
      return NULL;
   }

   const cJSON *jnodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
   if (!jnodes || !cJSON_IsArray(jnodes) || cJSON_GetArraySize(jnodes) == 0)
   {
      snprintf(err, errlen, "workflow has no 'nodes'");
      cJSON_Delete(root);
      return NULL;
   }

   wfe_def_t *def = calloc(1, sizeof *def);
   if (!def)
   {
      cJSON_Delete(root);
      return NULL;
   }
   def->raw = root;
   copy_str(def->name, sizeof def->name, obj_str(root, "name"));
   if (!def->name[0])
      copy_str(def->name, sizeof def->name, "unnamed");

   /* primary-as-manager (S0): top-level `enforced: true` marks a workflow the
    * router may bind a session to for substantive change. Accept bool/int/string
    * (YAML emitters vary), matching wfe_autonomy's `optional` reader. */
   const cJSON *jenf = cJSON_GetObjectItemCaseSensitive(root, "enforced");
   def->enforced =
       (jenf && (cJSON_IsTrue(jenf) || (cJSON_IsNumber(jenf) && jenf->valuedouble != 0) ||
                 (cJSON_IsString(jenf) && strcasecmp(jenf->valuestring, "true") == 0)))
           ? 1
           : 0;

   int n = cJSON_GetArraySize(jnodes);
   def->nodes = calloc((size_t)n, sizeof(wfe_node_t));
   if (!def->nodes)
   {
      wfe_def_free(def);
      return NULL;
   }
   const cJSON *jn = NULL;
   int i = 0;
   cJSON_ArrayForEach(jn, jnodes)
   {
      if (parse_node(&def->nodes[i], jn, err, errlen) != 0)
      {
         wfe_def_free(def);
         return NULL;
      }
      i++;
   }
   def->n_nodes = n;

   copy_str(def->start, sizeof def->start, obj_str(root, "start"));
   if (!def->start[0])
      copy_str(def->start, sizeof def->start, def->nodes[0].id);
   return def;
}

wfe_def_t *wfe_def_load_file(const char *path, char *err, size_t errlen)
{
   FILE *f = fopen(path, "rb");
   if (!f)
   {
      snprintf(err, errlen, "cannot open '%s'", path ? path : "(null)");
      return NULL;
   }
   fseek(f, 0, SEEK_END);
   long sz = ftell(f);
   if (sz < 0)
   {
      fclose(f);
      snprintf(err, errlen, "cannot size '%s'", path);
      return NULL;
   }
   fseek(f, 0, SEEK_SET);
   char *buf = malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   size_t rd = fread(buf, 1, (size_t)sz, f);
   buf[rd] = '\0';
   fclose(f);
   wfe_def_t *def = wfe_def_parse(buf, err, errlen);
   free(buf);
   return def;
}

void wfe_def_free(wfe_def_t *def)
{
   if (!def)
      return;
   free(def->nodes);
   if (def->raw)
      cJSON_Delete(def->raw);
   free(def);
}

const wfe_node_t *wfe_def_node(const wfe_def_t *def, const char *id)
{
   if (!def || !id)
      return NULL;
   for (int i = 0; i < def->n_nodes; i++)
      if (strcmp(def->nodes[i].id, id) == 0)
         return &def->nodes[i];
   return NULL;
}

wfe_gate_reject_t wfe_gate_reject_target(const wfe_def_t *def, const char *gate_id,
                                         char *out_target, size_t out_n)
{
   if (!def || !gate_id || !gate_id[0])
      return WFE_GATE_REJECT_ERR;
   const wfe_node_t *node = wfe_def_node(def, gate_id);
   if (!node)
      return WFE_GATE_REJECT_TERMINAL; /* unknown gate -> safe default (terminal) */
   /* Opt-in flag in the gate's params. */
   const cJSON *rr =
       node->params ? cJSON_GetObjectItemCaseSensitive(node->params, "retry_on_reject") : NULL;
   if (!cJSON_IsTrue(rr))
      return WFE_GATE_REJECT_TERMINAL;
   /* Must have somewhere to loop back to, and that target must still resolve to a
    * real node (a removed/renamed on_fail degrades safely to terminal, never a
    * dangling stage — this also bounds the pause->reject def-staleness window). */
   if (!node->on_fail[0] || !wfe_def_node(def, node->on_fail))
      return WFE_GATE_REJECT_TERMINAL;
   if (out_target && out_n)
   {
      if (strlen(node->on_fail) >= out_n)
         return WFE_GATE_REJECT_ERR; /* would truncate -> refuse rather than mis-route */
      snprintf(out_target, out_n, "%s", node->on_fail);
   }
   return WFE_GATE_REJECT_RETRY;
}

int wfe_node_max_iters(const wfe_node_t *n)
{
   const cJSON *m =
       (n && n->params) ? cJSON_GetObjectItemCaseSensitive(n->params, "max_iters") : NULL;
   if (cJSON_IsNumber(m) && m->valueint > 0)
      return m->valueint;
   return WFE_DEFAULT_MAX_ITERS;
}

wfe_on_max_t wfe_node_on_max(const wfe_node_t *n)
{
   const cJSON *o = (n && n->params) ? cJSON_GetObjectItemCaseSensitive(n->params, "on_max") : NULL;
   if (cJSON_IsString(o) && o->valuestring)
   {
      if (strcmp(o->valuestring, "fail") == 0)
         return WFE_ON_MAX_FAIL;
      if (strcmp(o->valuestring, "pass") == 0)
         return WFE_ON_MAX_PASS;
   }
   return WFE_ON_MAX_HUMAN;
}
