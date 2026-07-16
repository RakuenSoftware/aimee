/* session_compact.c: structured session compaction for long-running conversations.
 *
 * See headers/session_compact.h for the public API and design rationale.
 */
#include "aimee.h"
#include "session_compact.h"
#include "headers/compact_prune.h"
#include "agent_protocol.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ helpers */

static int resolve_warn_pct(const session_compact_config_t *cfg)
{
   return (cfg && cfg->warn_pct > 0) ? cfg->warn_pct : SESSION_COMPACT_DEFAULT_WARN_PCT;
}

static int resolve_compact_pct(const session_compact_config_t *cfg)
{
   return (cfg && cfg->compact_pct > 0) ? cfg->compact_pct : SESSION_COMPACT_DEFAULT_COMPACT_PCT;
}

static int resolve_retain_tail(const session_compact_config_t *cfg)
{
   return (cfg && cfg->retain_tail > 0) ? cfg->retain_tail : SESSION_COMPACT_RETAIN_TAIL;
}

/* Estimate token count from serialised length (4 chars ≈ 1 token). */
static int chars_to_tokens(size_t chars)
{
   return (int)((chars + 3) / 4);
}

/* Extract the text content from a single message item.
 * Handles both plain string content and array content (Anthropic multi-part).
 * Returns a pointer to the static string (do not free) or NULL. */
static const char *msg_text(const cJSON *msg)
{
   if (!msg)
      return NULL;
   cJSON *content = cJSON_GetObjectItem(msg, "content");
   if (!content)
      return NULL;
   if (cJSON_IsString(content))
      return content->valuestring;
   /* For array content, try to extract first text block */
   if (cJSON_IsArray(content))
   {
      cJSON *block;
      cJSON_ArrayForEach(block, content)
      {
         const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
         if (type && strcmp(type, "text") == 0)
         {
            const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(block, "text"));
            if (text)
               return text;
         }
      }
   }
   return NULL;
}

/* Return 1 if a message contains a tool call (assistant tool-use step). */
static int msg_is_tool_call(const cJSON *msg)
{
   if (!msg)
      return 0;
   /* OpenAI: tool_calls array present */
   cJSON *tc = cJSON_GetObjectItem(msg, "tool_calls");
   if (tc && cJSON_IsArray(tc) && cJSON_GetArraySize(tc) > 0)
      return 1;
   /* Anthropic: content array with a tool_use block */
   cJSON *content = cJSON_GetObjectItem(msg, "content");
   if (content && cJSON_IsArray(content))
   {
      cJSON *block;
      cJSON_ArrayForEach(block, content)
      {
         const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
         if (type && strcmp(type, "tool_use") == 0)
            return 1;
      }
   }
   return 0;
}

/* Return 1 if a message is a tool result (user-role tool result step). */
static int msg_is_tool_result(const cJSON *msg)
{
   if (!msg)
      return 0;
   const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
   if (!role || strcmp(role, "tool") == 0)
      return 1; /* OpenAI role=tool */
   /* Anthropic: role=user with content array containing tool_result blocks */
   if (strcmp(role, "user") == 0)
   {
      cJSON *content = cJSON_GetObjectItem(msg, "content");
      if (content && cJSON_IsArray(content))
      {
         cJSON *block;
         cJSON_ArrayForEach(block, content)
         {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (type && strcmp(type, "tool_result") == 0)
               return 1;
         }
      }
   }
   return 0;
}

/* ------------------------------------------------------------------ token estimation */

int session_compact_estimate_tokens(cJSON *messages)
{
   if (!messages || !cJSON_IsArray(messages))
      return 0;

   char *serialised = cJSON_PrintUnformatted(messages);
   if (!serialised)
      return 0;

   int tokens = chars_to_tokens(strlen(serialised));
   free(serialised);
   return tokens;
}

/* ------------------------------------------------------------------ pressure check */

int session_compact_pressure(int prompt_tokens, int completion_tokens, int context_window,
                             const session_compact_config_t *cfg)
{
   if (context_window <= 0)
      return SESSION_PRESSURE_OK;

   int total = prompt_tokens + completion_tokens;
   if (total <= 0)
      return SESSION_PRESSURE_OK;

   int pct = (int)(((long long)total * 100) / context_window);
   int compact_pct = resolve_compact_pct(cfg);
   int warn_pct = resolve_warn_pct(cfg);

   if (pct >= compact_pct)
      return SESSION_PRESSURE_COMPACT;
   if (pct >= warn_pct)
      return SESSION_PRESSURE_WARN;
   return SESSION_PRESSURE_OK;
}

/* ------------------------------------------------------------------ summary builder */

/* Append up to max_chars of text to buf at *pos (bounded by cap).
 * Truncates with "..." if the text exceeds max_chars. */
static void append_truncated(char *buf, size_t *pos, size_t cap, const char *text, size_t max_chars)
{
   if (!text || *pos >= cap)
      return;
   size_t tlen = strlen(text);
   size_t emit = (tlen > max_chars) ? max_chars : tlen;
   size_t remaining = cap - *pos - 1;
   if (emit > remaining)
      emit = remaining;
   memcpy(buf + *pos, text, emit);
   *pos += emit;
   if (tlen > max_chars && *pos + 4 < cap)
   {
      memcpy(buf + *pos, "...", 3);
      *pos += 3;
   }
   buf[*pos] = '\0';
}

static void append_format(char *buf, size_t *pos, size_t cap, const char *fmt, ...)
{
   if (!buf || !pos || *pos >= cap || !fmt)
      return;

   va_list ap;
   va_start(ap, fmt);
   int n = vsnprintf(buf + *pos, cap - *pos, fmt, ap);
   va_end(ap);

   if (n > 0)
      *pos += (size_t)n < cap - *pos ? (size_t)n : cap - *pos - 1;
}

static int token_looks_like_path(const char *tok)
{
   if (!tok || !tok[0])
      return 0;
   if (strchr(tok, '/') || strchr(tok, '\\'))
      return 1;
   static const char *exts[] = {".c",   ".h",  ".py",  ".js",   ".ts",   ".tsx",
                                ".jsx", ".go", ".rs",  ".java", ".json", ".yaml",
                                ".yml", ".md", ".sql", ".sh",   ".txt",  NULL};
   for (int i = 0; exts[i]; i++)
   {
      size_t len = strlen(tok);
      size_t ext_len = strlen(exts[i]);
      if (len > ext_len && strcmp(tok + len - ext_len, exts[i]) == 0)
         return 1;
   }
   return 0;
}

static int array_contains_string(cJSON *arr, const char *value)
{
   if (!arr || !cJSON_IsArray(arr) || !value || !value[0])
      return 0;
   cJSON *item = NULL;
   cJSON_ArrayForEach(item, arr)
   {
      const char *s = cJSON_GetStringValue(item);
      if (s && strcmp(s, value) == 0)
         return 1;
   }
   return 0;
}

static void flashback_add_unique(cJSON *arr, const char *value, size_t max_len)
{
   if (!arr || !value || !value[0] || array_contains_string(arr, value))
      return;
   char trimmed[256];
   snprintf(trimmed, sizeof(trimmed), "%.*s", (int)max_len, value);
   cJSON_AddItemToArray(arr, cJSON_CreateString(trimmed));
}

static void flashback_extract_from_text(cJSON *files, cJSON *errors, cJSON *decisions,
                                        const char *text)
{
   if (!text || !text[0])
      return;

   char lower[1024];
   snprintf(lower, sizeof(lower), "%s", text);
   for (size_t i = 0; lower[i]; i++)
      lower[i] = (char)tolower((unsigned char)lower[i]);

   if (strstr(lower, "error") || strstr(lower, "failed") || strstr(lower, "failure") ||
       strstr(lower, "exception") || strstr(lower, "traceback") || strstr(lower, "denied"))
      flashback_add_unique(errors, text, 180);

   if (strstr(lower, "decided") || strstr(lower, "decision") || strstr(lower, "choose") ||
       strstr(lower, "chosen") || strstr(lower, "agreed") || strstr(lower, "will use") ||
       strstr(lower, "use ") || strstr(lower, "using "))
      flashback_add_unique(decisions, text, 180);

   char copy[1024];
   snprintf(copy, sizeof(copy), "%s", text);
   const char *delims = " \t\r\n,;:()[]{}<>\"'";
   char *save = NULL;
   for (char *tok = strtok_r(copy, delims, &save); tok; tok = strtok_r(NULL, delims, &save))
   {
      if (token_looks_like_path(tok))
         flashback_add_unique(files, tok, 160);
   }
}

static cJSON *flashback_build(const cJSON *messages, int start_idx, int end_idx)
{
   cJSON *root = NULL;
   cJSON *files = NULL;
   cJSON *errors = NULL;
   cJSON *decisions = NULL;

   root = cJSON_CreateObject();
   files = cJSON_CreateArray();
   errors = cJSON_CreateArray();
   decisions = cJSON_CreateArray();
   if (!root || !files || !errors || !decisions)
      goto done;

   cJSON_AddItemToObject(root, "files_modified", files);
   cJSON_AddItemToObject(root, "errors_encountered", errors);
   cJSON_AddItemToObject(root, "decisions_made", decisions);
   files = errors = decisions = NULL;

   for (int i = start_idx; i < end_idx; i++)
   {
      cJSON *msg = cJSON_GetArrayItem((cJSON *)messages, i);
      flashback_extract_from_text(cJSON_GetObjectItem(root, "files_modified"),
                                  cJSON_GetObjectItem(root, "errors_encountered"),
                                  cJSON_GetObjectItem(root, "decisions_made"), msg_text(msg));
   }

   return root;

done:
   cJSON_Delete(root);
   cJSON_Delete(files);
   cJSON_Delete(errors);
   cJSON_Delete(decisions);
   return NULL;
}

static void append_array_items(char *buf, size_t *pos, size_t cap, cJSON *arr,
                               const char *empty_line)
{
   if (!arr || !cJSON_IsArray(arr) || cJSON_GetArraySize(arr) == 0)
   {
      append_format(buf, pos, cap, "- %s\n\n", empty_line ? empty_line : "None recorded.");
      return;
   }

   cJSON *item = NULL;
   cJSON_ArrayForEach(item, arr)
   {
      const char *s = cJSON_GetStringValue(item);
      if (!s || !s[0])
         continue;
      append_format(buf, pos, cap, "- ");
      append_truncated(buf, pos, cap, s, 220);
      append_format(buf, pos, cap, "\n");
   }
   append_format(buf, pos, cap, "\n");
}

static void append_flashback_json(char *buf, size_t *pos, size_t cap, cJSON *root)
{
   char *json = root ? cJSON_PrintUnformatted(root) : NULL;
   if (json && json[0])
      append_format(buf, pos, cap, "Flashback JSON: %s\n\n", json);
   free(json);
}

/* Build a structured text summary of messages[start_idx .. end_idx-1].
 * Writes the result into buf (length SESSION_COMPACT_SUMMARY_MAX). */
static void build_summary(const cJSON *messages, int start_idx, int end_idx, char *buf,
                          size_t buf_len)
{
   if (!buf || buf_len == 0)
      return;

   int n = cJSON_GetArraySize(messages);
   if (start_idx < 0)
      start_idx = 0;
   if (end_idx > n)
      end_idx = n;

   /* We build into a local buffer then copy, to avoid partial writes confusing callers */
   char tmp[SESSION_COMPACT_SUMMARY_MAX];
   size_t pos = 0;

   int n_summarised = end_idx - start_idx;
   const char *first_user_text = NULL;
   const char *last_user_text = NULL;
   const char *last_assistant_text = NULL;
   int tool_call_count = 0;
   int assistant_count = 0;

   for (int i = start_idx; i < end_idx; i++)
   {
      cJSON *msg = cJSON_GetArrayItem(messages, i);
      const char *role = cJSON_GetStringValue(cJSON_GetObjectItem(msg, "role"));
      if (role && strcmp(role, "user") == 0 && !msg_is_tool_result(msg))
      {
         const char *text = msg_text(msg);
         if (text && text[0])
         {
            if (!first_user_text)
               first_user_text = text;
            last_user_text = text;
         }
      }
      if (!role)
         continue;
      if (msg_is_tool_call(msg))
      {
         tool_call_count++;
      }
      else if (strcmp(role, "assistant") == 0)
      {
         const char *text = msg_text(msg);
         if (text && text[0])
         {
            assistant_count++;
            last_assistant_text = text;
         }
      }
   }

   cJSON *flashback = flashback_build(messages, start_idx, end_idx);
   cJSON *files = flashback ? cJSON_GetObjectItem(flashback, "files_modified") : NULL;
   cJSON *errors = flashback ? cJSON_GetObjectItem(flashback, "errors_encountered") : NULL;
   cJSON *decisions = flashback ? cJSON_GetObjectItem(flashback, "decisions_made") : NULL;

   append_format(tmp, &pos, sizeof(tmp), "[CONTEXT COMPACTION — REFERENCE ONLY]\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Active Task\n");
   append_format(tmp, &pos, sizeof(tmp), "- ");
   append_truncated(tmp, &pos, sizeof(tmp), first_user_text ? first_user_text : "Unknown.", 320);
   append_format(tmp, &pos, sizeof(tmp), "\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Goal\n");
   append_format(tmp, &pos, sizeof(tmp),
                 "- Preserve the older conversation state so the agent can continue after "
                 "compaction.\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Constraints\n");
   append_format(tmp, &pos, sizeof(tmp),
                 "- Treat this summary as reference only; newer explicit user and system "
                 "instructions still take priority.\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Completed Actions\n");
   append_format(tmp, &pos, sizeof(tmp), "- compacted %d messages into this reference summary.\n",
                 n_summarised);
   append_format(tmp, &pos, sizeof(tmp), "- Observed %d assistant turn%s", assistant_count,
                 assistant_count == 1 ? "" : "s");
   if (tool_call_count > 0)
      append_format(tmp, &pos, sizeof(tmp), " and %d tool call%s", tool_call_count,
                    tool_call_count == 1 ? "" : "s");
   append_format(tmp, &pos, sizeof(tmp), " in the summarised range.\n");
   if (last_assistant_text && last_assistant_text[0])
   {
      append_format(tmp, &pos, sizeof(tmp), "- Latest assistant note: ");
      append_truncated(tmp, &pos, sizeof(tmp), last_assistant_text, 512);
      append_format(tmp, &pos, sizeof(tmp), "\n");
   }
   append_format(tmp, &pos, sizeof(tmp), "\n");

   append_format(tmp, &pos, sizeof(tmp), "## Active State\n");
   append_format(tmp, &pos, sizeof(tmp),
                 "- First message and retained tail messages remain verbatim outside this "
                 "summary.\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## In Progress\n");
   append_format(tmp, &pos, sizeof(tmp),
                 "- Continue from the retained tail, using this summary for older context.\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Blocked\n");
   append_array_items(tmp, &pos, sizeof(tmp), errors, "None recorded.");

   append_format(tmp, &pos, sizeof(tmp), "## Key Decisions\n");
   append_array_items(tmp, &pos, sizeof(tmp), decisions, "None recorded.");

   append_format(tmp, &pos, sizeof(tmp), "## Resolved Questions\n");
   append_format(tmp, &pos, sizeof(tmp), "- None recorded.\n\n");

   append_format(tmp, &pos, sizeof(tmp), "## Pending User Asks\n");
   if (last_user_text && last_user_text[0])
   {
      append_format(tmp, &pos, sizeof(tmp), "- ");
      append_truncated(tmp, &pos, sizeof(tmp), last_user_text, 320);
      append_format(tmp, &pos, sizeof(tmp), "\n\n");
   }
   else
   {
      append_format(tmp, &pos, sizeof(tmp), "- Review the retained tail for the latest ask.\n\n");
   }

   append_format(tmp, &pos, sizeof(tmp), "## Relevant Files\n");
   append_array_items(tmp, &pos, sizeof(tmp), files, "None recorded.");
   append_flashback_json(tmp, &pos, sizeof(tmp), flashback);
   cJSON_Delete(flashback);

   /* Copy to caller's buffer */
   size_t copy_len = pos < buf_len - 1 ? pos : buf_len - 1;
   memcpy(buf, tmp, copy_len);
   buf[copy_len] = '\0';
}

/* --------------------------------------------- pre-boundary signature capture */

/* Tools whose repetition after a compaction boundary can ONLY mean the agent
 * lost context and is recovering it. Deliberately NOT derived from toolset.c's
 * `readonly` set, for two reasons:
 *
 *  1. `readonly` is wrong here: it bundles `verify`, `test` and `env_get`. An
 *     agent re-running `test` after an edit is making progress, not
 *     re-deriving — counting it would be a false positive.
 *  2. `readonly`/`core`/`git` are product surfaces. If someone adds a tool to
 *     `core` to fix a UX gap, this metric would silently change meaning and
 *     move a committed baseline with no reviewer ever seeing a metric change.
 *
 * The overlap with `core`+`git` is incidental, not shared knowledge. Editing
 * this list is a deliberate, reviewable change to what the metric means. */
static int compact_tool_is_readonly(const char *name)
{
   static const char *const k_readonly[] = {"read_file",     "list_files",  "grep",
                                            "code_search",   "find_symbol", "git_status",
                                            "git_log",       "git_diff",    "tool_output_get",
                                            "search_memory", NULL};
   if (!name || !name[0])
      return 0;
   for (int i = 0; k_readonly[i]; i++)
      if (strcmp(name, k_readonly[i]) == 0)
         return 1;
   return 0;
}

static uint64_t compact_fnv1a(const char *data)
{
   uint64_t h = 1469598103934665603ULL;
   for (const unsigned char *p = (const unsigned char *)(data ? data : ""); *p; p++)
   {
      h ^= (uint64_t)*p;
      h *= 1099511628211ULL;
   }
   return h;
}

int session_compact_tool_sig(const char *name, const char *args, char *out, size_t out_len)
{
   if (!out || out_len == 0 || !compact_tool_is_readonly(name))
      return 0;
   snprintf(out, out_len, "%.40s:%016llx", name, (unsigned long long)compact_fnv1a(args));
   return 1;
}

/* Record "<name>:<16-hex hash of args>" if read-only and not already present.
 * Deduplicates: the same lookup made twice pre-boundary is one fact, and would
 * otherwise inflate the count. */
static void add_readonly_sig(session_compact_result_t *out, const char *name, const char *args)
{
   char sig[SESSION_COMPACT_SIG_LEN];
   if (!session_compact_tool_sig(name, args, sig, sizeof(sig)))
      return;

   for (int i = 0; i < out->readonly_sig_count; i++)
      if (strcmp(out->readonly_sigs[i], sig) == 0)
         return; /* already recorded */

   if (out->readonly_sig_count >= SESSION_COMPACT_MAX_SIGS)
   {
      out->readonly_sigs_dropped++; /* never silently truncate */
      return;
   }
   snprintf(out->readonly_sigs[out->readonly_sig_count], SESSION_COMPACT_SIG_LEN, "%s", sig);
   out->readonly_sig_count++;
}

/* Walk messages[start, end) — the range about to be discarded — and record the
 * read-only tool calls found there. Handles both wire shapes:
 *   OpenAI:    {role:"assistant", tool_calls:[{function:{name, arguments}}]}
 *   Anthropic: {role:"assistant", content:[{type:"tool_use", name, input}]} */
static void capture_readonly_sigs(cJSON *messages, int start, int end,
                                  session_compact_result_t *out)
{
   for (int i = start; i < end; i++)
   {
      cJSON *msg = cJSON_GetArrayItem(messages, i);
      if (!msg || !cJSON_IsObject(msg))
         continue;

      /* OpenAI chat shape */
      cJSON *tool_calls = cJSON_GetObjectItem(msg, "tool_calls");
      if (cJSON_IsArray(tool_calls))
      {
         cJSON *call = NULL;
         cJSON_ArrayForEach(call, tool_calls)
         {
            cJSON *fn = cJSON_GetObjectItem(call, "function");
            if (!fn)
               continue;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "name"));
            const char *args = cJSON_GetStringValue(cJSON_GetObjectItem(fn, "arguments"));
            add_readonly_sig(out, name, args ? args : "");
         }
      }

      /* Anthropic content-block shape */
      cJSON *content = cJSON_GetObjectItem(msg, "content");
      if (cJSON_IsArray(content))
      {
         cJSON *block = NULL;
         cJSON_ArrayForEach(block, content)
         {
            const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(block, "type"));
            if (!type || strcmp(type, "tool_use") != 0)
               continue;
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(block, "name"));
            cJSON *input = cJSON_GetObjectItem(block, "input");
            char *args = input ? cJSON_PrintUnformatted(input) : NULL;
            add_readonly_sig(out, name, args ? args : "");
            if (args)
               free(args);
         }
      }
   }
}

/* ------------------------------------------------------------------ public API */

int session_compact(cJSON *messages, const session_compact_config_t *cfg,
                    session_compact_result_t *out)
{
   session_compact_result_t local_out;
   memset(&local_out, 0, sizeof(local_out));

   if (!messages || !cJSON_IsArray(messages))
   {
      if (out)
         *out = local_out;
      return -1;
   }

   int n = cJSON_GetArraySize(messages);
   local_out.messages_before = n;

   int retain = resolve_retain_tail(cfg);

   /* Need at least: 1 kept-first + enough to summarise + retain_tail.
    * If there's nothing meaningful to summarise, skip. */
   int min_to_compact = 1 + 1 + retain; /* first + at least 1 summarised + tail */
   if (n < min_to_compact)
   {
      local_out.messages_after = n;
      if (out)
         *out = local_out;
      return 0; /* nothing to do */
   }

   /* Step 1: repair orphaned tool pairs */
   local_out.repairs = message_history_repair(messages);

   /* Refresh count after repair (repair may add synthetic results) */
   n = cJSON_GetArraySize(messages);

   /* Step 2: determine the range to summarise.
    * Always keep: messages[0] (system/first-user) and the last `retain` messages.
    * Summarise: messages[1 .. n-retain-1]. */
   int summary_start = 1;        /* first message after the anchor */
   int summary_end = n - retain; /* exclusive: last `retain` messages are kept */

   if (summary_end <= summary_start)
   {
      /* Not enough messages to summarise anything */
      local_out.messages_after = n;
      if (out)
         *out = local_out;
      return 0;
   }

   /* Step 2a: capture the read-only tool signatures about to be destroyed.
    * Must run before Step 2b/Step 5 touch the range: this is the only moment
    * the pre-boundary tool history exists. Pure — it only fills `local_out`. */
   capture_readonly_sigs(messages, summary_start, summary_end, &local_out);

   /* Step 2b: prune large tool-result blobs before summarisation.
    * The pruned messages are about to be deleted (Step 5), so in-place
    * replacement is safe and only affects what build_summary() sees. */
   compact_prune_tool_results(messages, summary_start, summary_end, 0);

   /* Step 3: build summary of messages[summary_start..summary_end) */
   build_summary(messages, summary_start, summary_end, local_out.summary,
                 sizeof(local_out.summary));

   /* Step 4: insert a boundary marker message right after messages[0].
    * We build the JSON object first, then splice it in. */
   cJSON *boundary = cJSON_CreateObject();
   if (!boundary)
   {
      local_out.messages_after = n;
      if (out)
         *out = local_out;
      return -1;
   }
   cJSON_AddStringToObject(boundary, "role", "user");
   cJSON_AddStringToObject(boundary, "content", local_out.summary);
   /* Mark it so callers can identify it */
   cJSON_AddBoolToObject(boundary, "_compaction_boundary", cJSON_True);

   /* Step 5: remove messages[summary_start .. summary_end) from the array.
    * We do this by detaching items by index.  After each detach the array
    * shrinks by 1, so we always detach at the same index `summary_start`. */
   int removed = 0;
   for (int i = summary_start; i < summary_end; i++)
   {
      cJSON *item = cJSON_DetachItemFromArray(messages, summary_start);
      if (item)
      {
         cJSON_Delete(item);
         removed++;
      }
   }
   local_out.messages_removed = removed;

   /* Step 6: insert boundary after index 0 (now the array is shorter) */
   /* cJSON has no insert-at; we rebuild by detaching from 1 onward,
    * inserting boundary, then re-adding the tail items. */
   int after_remove = cJSON_GetArraySize(messages);
   /* Detach tail (everything after index 0) */
   cJSON *tail_items[256];
   int tail_count = 0;
   while (cJSON_GetArraySize(messages) > 1 && tail_count < 256)
   {
      tail_items[tail_count++] = cJSON_DetachItemFromArray(messages, 1);
   }
   /* Add boundary */
   cJSON_AddItemToArray(messages, boundary);
   /* Re-add tail */
   for (int i = 0; i < tail_count; i++)
   {
      if (tail_items[i])
         cJSON_AddItemToArray(messages, tail_items[i]);
   }

   /* Step 7: merge consecutive same-role messages */
   messages_compact_consecutive(messages);

   local_out.compacted = 1;
   local_out.messages_after = cJSON_GetArraySize(messages);

   (void)after_remove; /* used in debug builds via assert; suppress warning */

   if (out)
      *out = local_out;
   return 0;
}

int session_compact_focused(const char *topic, char *out, size_t out_len)
{
   if (!topic || !out || out_len == 0)
      return -1;

   /* Build a minimal 2-message synthetic conversation so build_summary()
    * populates ## Active Task from the user message (the focus topic).
    * A short assistant acknowledgement follows so the summary range is valid. */
   cJSON *messages = cJSON_CreateArray();
   if (!messages)
      return -1;

   cJSON *user_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(user_msg, "role", "user");
   cJSON_AddStringToObject(user_msg, "content", topic);
   cJSON_AddItemToArray(messages, user_msg);

   cJSON *asst_msg = cJSON_CreateObject();
   cJSON_AddStringToObject(asst_msg, "role", "assistant");
   cJSON_AddStringToObject(asst_msg, "content", "Acknowledged.");
   cJSON_AddItemToArray(messages, asst_msg);

   build_summary(messages, 0, 1, out, out_len);
   cJSON_Delete(messages);
   return 0;
}
