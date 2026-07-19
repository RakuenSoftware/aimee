/* roadmap_decompose.c: goal-to-roadmap JSON decomposition module.
 *
 * Pure-C: no agent/DB/network deps. Turns a goal into a validated roadmap
 * decomposition JSON by calling a caller-supplied model function and gating
 * every result through roadmap_validate_decomposition.
 */

#include "roadmap_decompose.h"
#include "roadmap.h"
#include "headers/dstr.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── prompt builder ──────────────────────────────────────────────────────── */

int roadmap_decompose_build_prompt(const char *goal, const char *planning_depth,
                                   const char *token_profile, const char *prior_err, char **out)
{
   if (!goal || !goal[0])
      return -1;

   if (!out)
      return -1;

   dstr_t s;
   dstr_init(&s);

   dstr_append_str(&s, "You are a roadmap decomposition assistant. Output ONLY a single JSON "
                       "object, no prose, no markdown fences, no explanations.\n\n"
                       "The JSON must match this schema:\n"
                       "{\n"
                       "  \"goal\": \"...\",\n"
                       "  \"planning_depth\": \"<depth>\",\n"
                       "  \"token_profile\": \"<profile>\",\n"
                       "  \"units\": [\n"
                       "    {\"local_id\":\"m1\",\"level\":\"milestone\",\"parent\":\"\","
                       "\"title\":\"...\",\"intent\":\"...\",\"depends_on\":[],"
                       "\"acceptance_criteria\":[\"...\"],\"tool_policy_mode\":\"planning\"},\n"
                       "    {\"local_id\":\"s1\",\"level\":\"slice\",\"parent\":\"m1\","
                       "\"title\":\"...\",\"intent\":\"...\",\"depends_on\":[],"
                       "\"acceptance_criteria\":[\"...\"]},\n"
                       "    {\"local_id\":\"t1\",\"level\":\"task\",\"parent\":\"s1\","
                       "\"title\":\"...\",\"intent\":\"...\",\"depends_on\":[],"
                       "\"owned_files\":[\"src/x.c\"],\"read_context\":[\"...\"],"
                       "\"acceptance_criteria\":[\"...\"],\"verification_commands\":[\"...\"],"
                       "\"tool_policy_mode\":\"execution\"}\n"
                       "  ]\n"
                       "}\n\n"
                       "Rules:\n"
                       "- Milestones have parent \"\".\n"
                       "- Each slice's parent is the local_id of a milestone.\n"
                       "- Each task's parent is the local_id of a slice.\n"
                       "- depends_on lists ONLY local_ids of same-parent siblings and must be "
                       "acyclic.\n"
                       "- Every task MUST have non-empty owned_files and non-empty "
                       "acceptance_criteria.\n"
                       "- tool_policy_mode is one of: planning, docs, execution.\n"
                       "- Output ONLY the JSON object, nothing else.\n\n");

   dstr_append_str(&s, "Goal: ");
   dstr_append_str(&s, goal);
   dstr_append_str(&s, "\n\n");

   const char *depth = planning_depth && planning_depth[0] ? planning_depth : "standard";
   const char *profile = token_profile && token_profile[0] ? token_profile : "balanced";

   dstr_appendf(&s, "planning_depth: %s\n", depth);
   dstr_appendf(&s, "token_profile: %s\n\n", profile);

   if (prior_err && prior_err[0])
   {
      dstr_append_str(&s, "Your previous attempt was rejected by the validator: ");
      dstr_append_str(&s, prior_err);
      dstr_append_str(&s, ". Fix it and output only the corrected JSON object.\n");
   }

   *out = strdup(dstr_cstr(&s));
   dstr_free(&s);

   return (*out) ? 0 : -1;
}

/* ── JSON extraction ─────────────────────────────────────────────────────── */

/* Walk text from the first '{', tracking brace depth while respecting
 * string literals and escape sequences inside them.
 * Returns 0 on success (start..end inclusive copied to *out, NUL-terminated);
 * -1 if no '{' found or braces never balance. Sets *out=NULL at entry. */
int roadmap_decompose_extract_json(const char *text, char **out)
{
   if (!out)
      return -1;
   *out = NULL;

   if (!text)
      return -1;

   /* Find first '{' */
   const char *start = strchr(text, '{');
   if (!start)
      return -1;

   int depth = 0;
   int in_string = 0;
   int prev_escaped = 0;
   const char *p = start;

   while (*p)
   {
      char c = *p;

      if (in_string)
      {
         /* Inside a string literal: only look for closing '"' that is not
          * preceded by an unescaped backslash. */
         if (c == '"' && !prev_escaped)
         {
            in_string = 0;
            prev_escaped = 0;
         }
         else if (c == '\\' && !prev_escaped)
         {
            /* Mark that the next char, if any, is escaped. */
            prev_escaped = 1;
         }
         else
         {
            prev_escaped = 0;
         }
      }
      else
      {
         /* Outside a string: check for opening a string or changing depth. */
         if (c == '"')
         {
            in_string = 1;
            prev_escaped = 0;
         }
         else if (c == '{')
         {
            depth++;
         }
         else if (c == '}')
         {
            depth--;
            if (depth == 0)
            {
               /* Found end of the balanced top-level object. */
               size_t len = (size_t)(p - start) + 1;
               char *buf = malloc(len + 1);
               if (!buf)
                  return -1;
               memcpy(buf, start, len);
               buf[len] = '\0';
               *out = buf;
               return 0;
            }
         }
      }

      p++;
   }

   /* Never reached depth 0 after opening brace. */
   return -1;
}

/* ── decompose run loop ──────────────────────────────────────────────────── */

int roadmap_decompose_run(const char *goal, const char *planning_depth, const char *token_profile,
                          roadmap_model_fn model, void *ud, int attempts, char **out_json,
                          char *err, size_t err_len)
{
   if (attempts <= 0)
      attempts = 1;

   if (!out_json)
      return -1;
   *out_json = NULL;

   if (!model)
   {
      if (err && err_len)
         snprintf(err, err_len, "no model function provided");
      return -1;
   }

   char prior[256] = "";

   for (int i = 0; i < attempts; i++)
   {
      char *prompt = NULL;
      if (roadmap_decompose_build_prompt(goal, planning_depth, token_profile,
                                         prior[0] ? prior : NULL, &prompt) != 0)
      {
         snprintf(prior, sizeof(prior), "prompt build failed");
         continue;
      }

      char *text = NULL;
      int mrc = model(prompt, &text, ud);
      free(prompt);
      prompt = NULL;

      if (mrc != 0 || !text)
      {
         free(text);
         snprintf(prior, sizeof(prior), "model call failed");
         continue;
      }

      char *json = NULL;
      if (roadmap_decompose_extract_json(text, &json) != 0 || !json)
      {
         free(text);
         free(json);
         snprintf(prior, sizeof(prior), "model did not emit a JSON object");
         continue;
      }

      free(text);
      text = NULL;

      char verr[256] = "";
      if (roadmap_validate_decomposition(json, verr, sizeof(verr)) == 0)
      {
         *out_json = json;
         return 0;
      }

      snprintf(prior, sizeof(prior), "%s", verr[0] ? verr : "validation failed");
      free(json);
      json = NULL;
   }

   if (err && err_len)
   {
      snprintf(err, err_len, "decomposition failed after %d attempt(s): %s", attempts,
               prior[0] ? prior : "unknown");
   }

   return -1;
}