/* wfe_router.c -- pure request->workflow routing core (S1). See wfe_router.h.
 * No I/O, no LLM, no filesystem: catalog is injected, classifier id is passed
 * in. Only <ctype.h>/<string.h>. */
#include "wfe_router.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* Change/imperative verbs. Presence means DEFER (let the classifier decide
 * converse-vs-work) -- NEVER an automatic route-to-change, so a question that
 * contains one of these ("how do I change X?") defers rather than routing to a
 * write workflow. Tunable; the S1 promotion gate measures precision before this
 * ever binds. */
static const char *CHANGE_VERBS[] = {
    "add",     "fix",       "change",   "implement", "refactor", "delete", "remove", "rename",
    "create",  "build",     "deploy",   "update",    "write",    "edit",   "patch",  "migrate",
    "install", "configure", "generate", "replace",   "revert",   NULL};

/* Explicit routing tags the user can type. */
static int starts_with_ci(const char *s, const char *pfx)
{
   while (*pfx)
   {
      if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pfx))
         return 0;
      s++;
      pfx++;
   }
   return 1;
}

/* whole-word (case-insensitive) search: `w` bounded by non-alnum on both sides. */
static int has_word(const char *hay, const char *w)
{
   size_t wl = strlen(w);
   for (const char *p = hay; *p; p++)
   {
      if (p != hay && isalnum((unsigned char)p[-1]))
         continue;
      size_t i = 0;
      while (i < wl && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)w[i]))
         i++;
      if (i == wl && !isalnum((unsigned char)p[i]))
         return 1;
   }
   return 0;
}

/* a code/path/diff token: a '/', a "```" fence, a backtick, or a ".<ext>" where
 * ext is 1-4 alnum (foo.c, x.py, a.json). Cheap over-approximation -> DEFER. */
static int has_code_token(const char *s)
{
   if (strchr(s, '/') || strstr(s, "```") || strchr(s, '`'))
      return 1;
   for (const char *p = s; *p; p++)
   {
      if (*p == '.' && isalpha((unsigned char)p[1]))
      {
         int k = 1;
         while (k <= 4 && isalnum((unsigned char)p[k]))
            k++;
         /* ext ends at a non-alnum boundary and had >=1 alnum */
         if (k > 1 && !isalnum((unsigned char)p[k]))
            return 1;
      }
   }
   return 0;
}

static int is_blank(const char *s)
{
   for (; s && *s; s++)
      if (!isspace((unsigned char)*s))
         return 0;
   return 1;
}

/* a valid workflow id: non-empty, [A-Za-z0-9_-] only, and short enough to fit
 * the id buffer without truncation. Ids are interpolated into the advisory-log
 * JSON and used as routing keys, so an unvalidated id (with a quote, control
 * char, slash, ...) would be a JSON-injection / routing hazard. */
int wfe_router_id_valid(const char *id)
{
   if (!id || !id[0])
      return 0;
   size_t l = 0;
   for (const char *p = id; *p; p++, l++)
   {
      char c = *p;
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '_' || c == '-'))
         return 0;
   }
   return l < WFE_ROUTER_ID_LEN;
}

int wfe_router_catalog_validate(const wfe_router_catalog_t *cat, char *err, size_t errlen)
{
   if (err && errlen)
      err[0] = '\0';
   if (!cat || cat->n <= 0)
   {
      snprintf(err, errlen, "empty router catalog");
      return -1;
   }
   int ndefault = 0;
   for (int i = 0; i < cat->n; i++)
   {
      const wfe_router_wf_t *w = &cat->wf[i];
      if (!wfe_router_id_valid(w->id))
      {
         snprintf(err, errlen, "catalog entry %d has an invalid id (allowed: A-Za-z0-9_-)", i);
         return -1;
      }
      for (int j = i + 1; j < cat->n; j++)
         if (strcmp(w->id, cat->wf[j].id) == 0)
         {
            snprintf(err, errlen, "duplicate workflow id '%s'", w->id);
            return -1;
         }
      if (w->is_default)
      {
         ndefault++;
         if (!w->read_only)
         {
            snprintf(err, errlen, "default workflow '%s' must be read-only (safe fallback)", w->id);
            return -1;
         }
      }
   }
   if (ndefault != 1)
   {
      snprintf(err, errlen, "catalog must declare exactly one default workflow (found %d)",
               ndefault);
      return -1;
   }
   if (!wfe_router_find(cat, "converse"))
   {
      snprintf(err, errlen, "catalog must include a 'converse' lane");
      return -1;
   }
   return 0;
}

const wfe_router_wf_t *wfe_router_find(const wfe_router_catalog_t *cat, const char *id)
{
   if (!cat || !id)
      return NULL;
   for (int i = 0; i < cat->n; i++)
      if (strcmp(cat->wf[i].id, id) == 0)
         return &cat->wf[i];
   return NULL;
}

const wfe_router_wf_t *wfe_router_default(const wfe_router_catalog_t *cat)
{
   if (!cat)
      return NULL;
   for (int i = 0; i < cat->n; i++)
      if (cat->wf[i].is_default)
         return &cat->wf[i];
   return NULL;
}

/* parse an explicit `use <name>` directive at the start of the message; returns
 * the token into buf or "" if none. */
static void parse_use(const char *msg, char *buf, size_t n)
{
   buf[0] = '\0';
   const char *p = msg;
   while (*p && isspace((unsigned char)*p))
      p++;
   if (!starts_with_ci(p, "use "))
      return;
   p += 4;
   while (*p && isspace((unsigned char)*p))
      p++;
   size_t k = 0;
   while (*p && !isspace((unsigned char)*p) && k + 1 < n)
      buf[k++] = *p++;
   buf[k] = '\0';
}

wfe_prefilter_outcome_t wfe_router_prefilter(const char *msg, const wfe_router_catalog_t *cat,
                                             char *matched_id, size_t idlen, char *reason,
                                             size_t rlen)
{
   if (matched_id && idlen)
      matched_id[0] = '\0';
   if (reason && rlen)
      reason[0] = '\0';
   if (is_blank(msg))
   {
      if (reason)
         snprintf(reason, rlen, "empty-message");
      return WFE_PREFILTER_CONVERSE;
   }
   /* explicit "just chat" -> converse */
   {
      const char *p = msg;
      while (*p && isspace((unsigned char)*p))
         p++;
      if (starts_with_ci(p, "just chat"))
      {
         if (reason)
            snprintf(reason, rlen, "explicit-just-chat");
         return WFE_PREFILTER_CONVERSE;
      }
   }
   /* explicit `use <name>`: validate against the catalog (exact, case-sensitive);
    * on miss DEFER (never fuzzy-match, never a raw path). */
   {
      char name[WFE_ROUTER_ID_LEN];
      parse_use(msg, name, sizeof name);
      if (name[0])
      {
         if (wfe_router_find(cat, name))
         {
            if (matched_id)
               snprintf(matched_id, idlen, "%s", name);
            if (reason)
               snprintf(reason, rlen, "explicit-use-name");
            return WFE_PREFILTER_NAMED;
         }
         if (reason)
            snprintf(reason, rlen, "unknown-use-name");
         return WFE_PREFILTER_DEFER; /* unknown name -> classifier/default, not fuzzy */
      }
   }
   /* high-confidence converse: no change verb AND no code/path/diff token. */
   {
      int verb = 0;
      for (int i = 0; CHANGE_VERBS[i]; i++)
         if (has_word(msg, CHANGE_VERBS[i]))
         {
            verb = 1;
            break;
         }
      int code = has_code_token(msg);
      if (!verb && !code)
      {
         if (reason)
            snprintf(reason, rlen, "no-change-signal");
         return WFE_PREFILTER_CONVERSE;
      }
      if (reason)
         snprintf(reason, rlen, verb ? "change-verb-defer" : "code-token-defer");
      return WFE_PREFILTER_DEFER;
   }
}

void wfe_router_decide(const char *msg, const wfe_router_catalog_t *cat, const char *classifier_id,
                       wfe_route_decision_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof *out);
   const wfe_router_wf_t *dflt = wfe_router_default(cat);
   const char *dflt_id = dflt ? dflt->id : "research";

   char named[WFE_ROUTER_ID_LEN] = "", reason[96] = "";
   wfe_prefilter_outcome_t pf =
       wfe_router_prefilter(msg, cat, named, sizeof named, reason, sizeof reason);

   if (pf == WFE_PREFILTER_NAMED)
   {
      snprintf(out->workflow_id, sizeof out->workflow_id, "%s", named);
      out->source = WFE_ROUTE_SRC_PREFILTER;
      out->user_provided_name = 1;
      snprintf(out->reason, sizeof out->reason, "%s", reason);
      return;
   }
   if (pf == WFE_PREFILTER_CONVERSE)
   {
      snprintf(out->workflow_id, sizeof out->workflow_id, "converse");
      out->source = WFE_ROUTE_SRC_PREFILTER;
      snprintf(out->reason, sizeof out->reason, "%s", reason);
      return;
   }
   /* DEFER: use the classifier id iff it is a real catalog id; else read-only
    * default (covers no-sample, timeout, error, and out-of-catalog output). */
   if (classifier_id && wfe_router_find(cat, classifier_id))
   {
      snprintf(out->workflow_id, sizeof out->workflow_id, "%s", classifier_id);
      out->source = WFE_ROUTE_SRC_CLASSIFIER;
      snprintf(out->reason, sizeof out->reason, "classifier");
      return;
   }
   snprintf(out->workflow_id, sizeof out->workflow_id, "%s", dflt_id);
   out->source = WFE_ROUTE_SRC_DEFAULT;
   snprintf(out->reason, sizeof out->reason,
            classifier_id ? "classifier-out-of-catalog" : "defer-no-classifier");
}

/* is `c` part of a workflow-id token? (ids are [A-Za-z0-9_-]) */
static int id_char(char c)
{
   return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
          c == '-';
}

void wfe_router_classify_prompt(const wfe_router_catalog_t *cat, char *buf, size_t n)
{
   if (!buf || !n)
      return;
   int off = snprintf(buf, n,
                      "You are a request router. Classify the user's request into EXACTLY ONE of "
                      "these workflow ids and reply with ONLY that id (no other text):\n");
   for (int i = 0; cat && i < cat->n && off > 0 && (size_t)off < n; i++)
   {
      const wfe_router_wf_t *w = &cat->wf[i];
      off +=
          snprintf(buf + off, n - (size_t)off, "- %s%s", w->id, w->read_only ? " (read-only)" : "");
      for (int t = 0; t < w->n_tags && (size_t)off < n; t++)
         off += snprintf(buf + off, n - (size_t)off, "%s%s", t == 0 ? " tags: " : ",", w->tags[t]);
      if ((size_t)off < n)
         off += snprintf(buf + off, n - (size_t)off, "\n");
   }
   if (off > 0 && (size_t)off < n)
   {
      const wfe_router_wf_t *df = wfe_router_default(cat);
      snprintf(buf + off, n - (size_t)off,
               "If none clearly fit, or the request is read-only/ambiguous, reply: %s\n",
               df ? df->id : "research");
   }
}

int wfe_router_parse_classification(const char *response, const wfe_router_catalog_t *cat,
                                    char *out_id, size_t n)
{
   if (out_id && n)
      out_id[0] = '\0';
   if (!response || !cat)
      return -1;
   /* Scan for a catalog id that appears bounded by non-id chars, and keep the
    * LAST such match: models are told to reply with only the id, but a reasoning
    * model may narrate first and put its answer at the end, so the final id token
    * is the most reliable signal (a clean single-token reply has last == first).
    * Catalog order is stable, so any residual tie is deterministic. */
   const char *found = NULL;
   for (const char *p = response; *p; p++)
   {
      if (p != response && id_char(p[-1]))
         continue;
      for (int i = 0; i < cat->n; i++)
      {
         const char *id = cat->wf[i].id;
         size_t l = strlen(id);
         if (strncmp(p, id, l) == 0 && !id_char(p[l]))
         {
            found = id;
            break;
         }
      }
   }
   if (!found)
      return -1;
   snprintf(out_id, n, "%s", found);
   return 0;
}

static const char *prefilter_name(wfe_prefilter_outcome_t o)
{
   switch (o)
   {
   case WFE_PREFILTER_CONVERSE:
      return "converse";
   case WFE_PREFILTER_NAMED:
      return "named";
   default:
      return "defer";
   }
}

static const char *source_name(wfe_route_source_t s)
{
   switch (s)
   {
   case WFE_ROUTE_SRC_PREFILTER:
      return "prefilter";
   case WFE_ROUTE_SRC_CLASSIFIER:
      return "classifier";
   default:
      return "default";
   }
}

void wfe_router_advisory_payload(const wfe_route_decision_t *d, wfe_prefilter_outcome_t prefilter,
                                 int sampled, double classifier_ms, char *buf, size_t n)
{
   /* structural features only -- NO raw message text. reason/id are router-
    * generated (safe id charset), so no JSON escaping is required here. */
   if (classifier_ms >= 0)
      snprintf(buf, n,
               "{\"workflow_id\":\"%s\",\"source\":\"%s\",\"prefilter\":\"%s\","
               "\"user_provided_name\":%s,\"sampled\":%s,\"reason\":\"%s\","
               "\"classifier_ms\":%.1f}",
               d->workflow_id, source_name(d->source), prefilter_name(prefilter),
               d->user_provided_name ? "true" : "false", sampled ? "true" : "false", d->reason,
               classifier_ms);
   else
      snprintf(buf, n,
               "{\"workflow_id\":\"%s\",\"source\":\"%s\",\"prefilter\":\"%s\","
               "\"user_provided_name\":%s,\"sampled\":%s,\"reason\":\"%s\"}",
               d->workflow_id, source_name(d->source), prefilter_name(prefilter),
               d->user_provided_name ? "true" : "false", sampled ? "true" : "false", d->reason);
}

int wfe_router_should_sample(const char *session_id, int turn_index, int one_in_n)
{
   if (one_in_n <= 1)
      return 1;
   /* FNV-1a over session_id + turn_index -> deterministic, no RNG. */
   unsigned int h = 2166136261u;
   for (const char *p = session_id ? session_id : ""; *p; p++)
      h = (h ^ (unsigned char)*p) * 16777619u;
   unsigned int t = (unsigned int)turn_index;
   for (int i = 0; i < 4; i++)
      h = (h ^ ((t >> (i * 8)) & 0xff)) * 16777619u;
   return (h % (unsigned int)one_in_n) == 0;
}
