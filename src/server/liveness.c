/* liveness.c: delegate liveness detection utilities.
 *
 * Pure helper functions; no I/O, no database access, no LLM calls.
 * See headers/liveness.h for the public API. */

#include "liveness.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tool_markup_line(const char *start, const char *end)
{
   const size_t tool_close_len = sizeof("</tool_call>") - 1;
   const size_t fn_close_len = sizeof("</function>") - 1;
   const size_t param_close_len = sizeof("</parameter>") - 1;
   const size_t tool_open_len = sizeof("<tool_call") - 1;
   const size_t fn_open_len = sizeof("<function=") - 1;
   const size_t param_open_len = sizeof("<parameter") - 1;
   const size_t invoke_open_len = sizeof("<invoke") - 1;
   const size_t invoke_close_len = sizeof("</invoke>") - 1;
   const size_t channel_call_len = sizeof("<|channel>call:") - 1;
   const size_t bracket_tool_open_len = sizeof("[TOOL_CALL]") - 1;
   const size_t bracket_tools_open_len = sizeof("[TOOL_CALLS]") - 1;
   const size_t bracket_tool_close_len = sizeof("[/TOOL_CALL]") - 1;

   while (start < end && isspace((unsigned char)*start))
      start++;
   while (end > start && isspace((unsigned char)end[-1]))
      end--;

   size_t len = (size_t)(end - start);
   if (len == 0)
      return 0;

   return (len == tool_close_len && strncmp(start, "</tool_call>", tool_close_len) == 0) ||
          (len == fn_close_len && strncmp(start, "</function>", fn_close_len) == 0) ||
          (len == param_close_len && strncmp(start, "</parameter>", param_close_len) == 0) ||
          (len >= tool_open_len && strncmp(start, "<tool_call", tool_open_len) == 0) ||
          (len >= fn_open_len && strncmp(start, "<function=", fn_open_len) == 0) ||
          (len >= param_open_len && strncmp(start, "<parameter", param_open_len) == 0) ||
          (len >= invoke_open_len && strncmp(start, "<invoke", invoke_open_len) == 0) ||
          (len == invoke_close_len && strncmp(start, "</invoke>", invoke_close_len) == 0) ||
          (len >= channel_call_len && strncmp(start, "<|channel>call:", channel_call_len) == 0) ||
          (len == bracket_tool_open_len &&
           strncmp(start, "[TOOL_CALL]", bracket_tool_open_len) == 0) ||
          (len >= bracket_tools_open_len &&
           strncmp(start, "[TOOL_CALLS]", bracket_tools_open_len) == 0) ||
          (len == bracket_tool_close_len &&
           strncmp(start, "[/TOOL_CALL]", bracket_tool_close_len) == 0);
}

static int diff_fragment_line(const char *start, const char *end)
{
   while (start < end && isspace((unsigned char)*start))
      start++;
   while (end > start && isspace((unsigned char)end[-1]))
      end--;
   if (end <= start)
      return 0;
   return (*start == '+' || *start == '-') && end - start > 1;
}

static int prose_line(const char *start, const char *end)
{
   int words = 0;
   int in_word = 0;
   int word_len = 0;
   int has_space = 0;

   for (const char *p = start; p < end; p++)
   {
      unsigned char ch = (unsigned char)*p;
      if (isspace(ch))
         has_space = 1;
      if (isalpha(ch))
      {
         if (!in_word)
         {
            in_word = 1;
            word_len = 0;
         }
         word_len++;
      }
      else
      {
         if (in_word && word_len >= 2)
            words++;
         in_word = 0;
         word_len = 0;
      }
   }
   if (in_word && word_len >= 2)
      words++;
   return has_space && words >= 3;
}

static int line_starts_with_any(const char *start, const char *end, const char *const *prefixes)
{
   while (start < end && isspace((unsigned char)*start))
      start++;
   for (int i = 0; prefixes[i]; i++)
   {
      size_t n = strlen(prefixes[i]);
      if ((size_t)(end - start) >= n && strncmp(start, prefixes[i], n) == 0)
         return 1;
   }
   return 0;
}

static int line_contains_ci(const char *start, const char *end, const char *needle)
{
   size_t needle_len = strlen(needle);
   if (needle_len == 0)
      return 1;
   for (const char *p = start; p + needle_len <= end; p++)
   {
      size_t i = 0;
      for (; i < needle_len; i++)
      {
         unsigned char a = (unsigned char)p[i];
         unsigned char b = (unsigned char)needle[i];
         if (tolower(a) != tolower(b))
            break;
      }
      if (i == needle_len)
         return 1;
   }
   return 0;
}

static int line_has_future_tool_intent(const char *start, const char *end)
{
   static const char *const intents[] = {"I'll ", "I will ", "I am going to ", "I'm going to ",
                                         NULL};
   if (!line_contains_ci(start, end, "inspect") && !line_contains_ci(start, end, "read") &&
       !line_contains_ci(start, end, "check") && !line_contains_ci(start, end, "explore") &&
       !line_contains_ci(start, end, "run") && !line_contains_ci(start, end, "search"))
      return 0;
   return line_starts_with_any(start, end, intents);
}

static int line_looks_like_shell_command(const char *start, const char *end)
{
   static const char *const commands[] = {
       "aimee ",   "./aimee ", "rg ",  "grep ",    "find ", "fd ",    "ls ",
       "sed ",     "cat ",     "git ", "sqlite3 ", "make ", "cmake ", "python ",
       "python3 ", "dd ",      "awk ", "jq ",      "head ", "tail ",  NULL};
   return line_starts_with_any(start, end, commands);
}

int liveness_is_empty_response(const char *content)
{
   if (!content)
      return 1;
   const char *p = content;
   while (*p)
   {
      if (!isspace((unsigned char)*p))
         return 0;
      p++;
   }
   return 1; /* all whitespace (or empty string) */
}

int liveness_is_degenerate_response(const char *content)
{
   if (liveness_is_empty_response(content))
      return 1;

   const char *trimmed = content;
   while (isspace((unsigned char)*trimmed))
      trimmed++;
   if (*trimmed == '$')
   {
      const char *p = trimmed + 1;
      while (isalnum((unsigned char)*p) || *p == '_' || *p == '-')
         p++;
      if (p > trimmed + 1 && *p == '{' && strchr(p + 1, '}'))
      {
         const char *end = strrchr(p + 1, '}') + 1;
         while (isspace((unsigned char)*end))
            end++;
         if (*end == '\0')
            return 1;
      }
   }
   if (strstr(trimmed, "[TOOL_CALL]") && strstr(trimmed, "[/TOOL_CALL]"))
      return 1;
   /* A complete <tool_call> block whose body carries JSON is a real, unexecuted
    * tool call leaked into the final answer (weak models sometimes narrate then
    * emit one). An *empty* <tool_call></tool_call> merely mentioned in prose is
    * fine, so require a "{" in the body to distinguish a leaked call from a
    * mention. For code roles the real calls were already executed upstream. */
   {
      const char *tc = strstr(trimmed, "<tool_call>");
      const char *tce = tc ? strstr(tc, "</tool_call>") : NULL;
      if (tce && memchr(tc, '{', (size_t)(tce - tc)))
         return 1;
   }

   int signal = 0;
   int alpha = 0;
   int digit = 0;
   int punctuation = 0;
   int nonspace = 0;
   unsigned char seen[256] = {0};
   int unique_nonspace = 0;
   int repeated_quote_lines = 0;
   int tool_markup_lines = 0;
   int diff_fragment_lines = 0;
   int prose_lines = 0;
   const char *line = content;
   for (const char *p = content;; p++)
   {
      unsigned char ch = (unsigned char)*p;
      if (isalnum(ch))
      {
         signal++;
         if (isalpha(ch))
            alpha++;
         else if (isdigit(ch))
            digit++;
      }
      else if (!isspace(ch))
         punctuation++;
      if (*p && !isspace(ch))
      {
         nonspace++;
         if (!seen[ch])
         {
            seen[ch] = 1;
            unique_nonspace++;
         }
      }

      if (*p == '\n' || *p == '\0')
      {
         const char *start = line;
         const char *end = p;
         while (start < end && isspace((unsigned char)*start))
            start++;
         while (end > start && isspace((unsigned char)end[-1]))
            end--;
         int is_tool_markup = tool_markup_line(start, end);
         int is_diff_fragment = diff_fragment_line(start, end);
         if (is_tool_markup)
            tool_markup_lines++;
         if (is_diff_fragment)
            diff_fragment_lines++;
         if (!is_tool_markup && !is_diff_fragment && prose_line(start, end))
            prose_lines++;
         if (end - start == 1 && (*start == '"' || *start == '\'' || *start == '`'))
            repeated_quote_lines++;
         line = p + 1;
      }
      if (*p == '\0')
         break;
   }

   if (signal == 0 && punctuation > 0)
      return 1;
   if (tool_markup_lines >= 2 && prose_lines == 0)
      return 1;
   if (diff_fragment_lines >= 2 && prose_lines == 0)
      return 1;
   if (repeated_quote_lines >= 8 && signal < 8)
      return 1;
   if (alpha == 0 && digit > 0 && punctuation > 0 && nonspace >= 32)
      return 1;
   if (nonspace >= 64 && unique_nonspace <= 4)
      return 1;
   return 0;
}

int liveness_is_unexecuted_tool_plan_response(const char *content)
{
   if (liveness_is_empty_response(content))
      return 0;

   int future_intent = 0;
   int fenced = 0;
   int in_fence = 0;
   int command_lines = 0;
   int prose_lines = 0;
   const char *line = content;
   for (const char *p = content;; p++)
   {
      if (*p == '\n' || *p == '\0')
      {
         const char *start = line;
         const char *end = p;
         while (start < end && isspace((unsigned char)*start))
            start++;
         while (end > start && isspace((unsigned char)end[-1]))
            end--;

         if ((end - start) >= 3 && strncmp(start, "```", 3) == 0)
         {
            in_fence = !in_fence;
            if (in_fence)
               fenced++;
         }
         else if (in_fence)
         {
            if (line_looks_like_shell_command(start, end))
               command_lines++;
         }
         else
         {
            if (line_has_future_tool_intent(start, end))
               future_intent = 1;
            if (prose_line(start, end))
               prose_lines++;
         }

         line = p + 1;
      }
      if (*p == '\0')
         break;
   }

   return future_intent && fenced > 0 && command_lines > 0 && prose_lines <= 2;
}

int liveness_reject_degenerate_response(char **response, char *error, size_t error_sz,
                                        const char *error_message)
{
   if (!response || !*response || !liveness_is_degenerate_response(*response))
      return 0;

   if (error && error_sz > 0)
      snprintf(error, error_sz, "%s",
               error_message
                   ? error_message
                   : "agent returned raw tool-call markup or another degenerate response");
   free(*response);
   *response = NULL;
   return 1;
}

void liveness_format_empty_diagnostic(const char *agent_name, char *buf, size_t bufsz)
{
   if (!buf || bufsz == 0)
      return;
   if (agent_name && agent_name[0])
      snprintf(buf, bufsz,
               "[LIVENESS ERROR] Delegate '%s' returned an empty response. "
               "No result was produced. The task may be incomplete.",
               agent_name);
   else
      snprintf(buf, bufsz,
               "[LIVENESS ERROR] Delegate returned an empty response. "
               "No result was produced. The task may be incomplete.");
}

void liveness_format_final_tool_call_diagnostic(const char *agent_name, const char *attempted_tool,
                                                const char *partial_text,
                                                const char *last_tool_name,
                                                const char *last_tool_result, int total_tool_calls,
                                                char *buf, size_t bufsz)
{
   if (!buf || bufsz == 0)
      return;

   const char *agent = (agent_name && agent_name[0]) ? agent_name : "delegate";
   const char *attempted = (attempted_tool && attempted_tool[0]) ? attempted_tool : "unknown";
   const char *last_tool = (last_tool_name && last_tool_name[0]) ? last_tool_name : "unknown";
   const char *last_result = (last_tool_result && last_tool_result[0]) ? last_tool_result : "empty";

   if (!liveness_is_degenerate_response(partial_text))
   {
      snprintf(buf, bufsz,
               "%.900s\n\n"
               "[LIVENESS NOTICE] Delegate '%s' reached the final response turn after %d "
               "completed tool call(s), but attempted disabled tool call '%.80s' instead of "
               "returning final text. No additional tools were run. Last completed tool: %.80s. "
               "Last result excerpt: %.500s",
               partial_text, agent, total_tool_calls, attempted, last_tool, last_result);
      return;
   }

   snprintf(buf, bufsz,
            "[LIVENESS NOTICE] Delegate '%s' reached the final response turn after %d completed "
            "tool call(s), but attempted disabled tool call '%.80s' instead of returning final "
            "text. No additional tools were run. Last completed tool: %.80s. Last result "
            "excerpt: %.500s\n\nTask may be incomplete; continue from the completed tool "
            "evidence.",
            agent, total_tool_calls, attempted, last_tool, last_result);
}

int liveness_circuit_breaker_tripped(int total_triggers)
{
   return total_triggers >= LIVENESS_REPEAT_ABORT_THRESHOLD;
}

liveness_final_response_mode_t
liveness_final_response_mode(int turn, int max_turns, int total_tool_calls, int final_after_turns)
{
   /* The hard boundary does not depend on prior tool use. The budget is ending
    * either way, and total_tool_calls counts calls that LANDED: a delegate
    * whose every attempt was denied by policy or emptied by compaction has done
    * nothing but call tools while this count stayed at zero. Gating the hard
    * turn on it let exactly that delegate spend all its turns and return
    * nothing, which is what "max turns exhausted without final response" is. */
   if (max_turns > 1 && turn >= max_turns - 1)
      return LIVENESS_FINAL_RESPONSE_HARD;

   /* The soft, role-policy threshold still does: nudging an agent that has not
    * done anything yet, while turns remain, is just noise. */
   if (total_tool_calls <= 0)
      return LIVENESS_FINAL_RESPONSE_NONE;

   if (final_after_turns > 0 && turn >= final_after_turns)
      return LIVENESS_FINAL_RESPONSE_SOFT;

   return LIVENESS_FINAL_RESPONSE_NONE;
}

int liveness_final_response_allows_tools(liveness_final_response_mode_t mode)
{
   return mode == LIVENESS_FINAL_RESPONSE_NONE;
}
