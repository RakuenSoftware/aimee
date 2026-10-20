/* session_compact.c: structured session compaction for long-running conversations.
 *
 * See headers/session_compact.h for the public API and design rationale.
 */
#include "aimee.h"
#include "session_compact.h"
#include "util.h"
#include "economizer.h" /* event-bus client for the Go-owned record path */
#include "cJSON.h"
#include <aimee/ir/session_event.h>
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

/* The JSON wrapper is a persistence boundary. Every consumer below reads the
 * owned canonical messages, never native message/content/tool-call shapes. */
typedef struct
{
   aimee_message_t *messages;
   int count;
} compact_history_t;

static void history_free(compact_history_t *history)
{
   for (int i = 0; i < history->count; i++)
      aimee_message_free(&history->messages[i]);
   free(history->messages);
   memset(history, 0, sizeof *history);
}

static int history_load(const cJSON *messages, compact_history_t *history)
{
   memset(history, 0, sizeof *history);
   if (!cJSON_IsArray(messages))
      return -1;
   int count = cJSON_GetArraySize(messages);
   if (count > AIMEE_IR_MAX_BLOCKS)
      return -1;
   history->messages = count ? calloc((size_t)count, sizeof *history->messages) : NULL;
   if (count && !history->messages)
      return -1;
   for (int i = 0; i < count; i++)
   {
      const cJSON *source = cJSON_GetArrayItem(messages, i);
      const cJSON *field;
      cJSON_ArrayForEach(field, source)
      {
         if (!field->string || (strcmp(field->string, "role") && strcmp(field->string, "blocks") &&
                                strcmp(field->string, "content_scalar") &&
                                strcmp(field->string, "_compaction_boundary")))
            goto fail;
      }
      if (aimee_ir_message_parse(source, &history->messages[i]))
         goto fail;
      history->count++;
   }
   aimee_request_t request = {.messages = history->messages, .n_messages = count};
   if (aimee_ir_request_admit(&request))
      goto fail;
   return 0;
fail:
   history_free(history);
   return -1;
}

static int msg_tool_count(const aimee_message_t *message)
{
   int count = 0;
   for (int i = 0; i < message->n_blocks; i++)
      count += message->blocks[i].type == AIMEE_BLK_TOOL_USE;
   return count;
}

static int msg_is_tool_result(const aimee_message_t *message)
{
   for (int i = 0; i < message->n_blocks; i++)
      if (message->blocks[i].type == AIMEE_BLK_TOOL_RESULT)
         return 1;
   return 0;
}

static char *msg_text(const aimee_message_t *message)
{
   aimee_response_t view = {.content = message->blocks, .n_content = message->n_blocks};
   return aimee_ir_response_text_alloc(&view);
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
   buf[*pos] = '\0';
   text_trim_partial_utf8(buf);
   *pos = strlen(buf);
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
   {
      *pos += (size_t)n < cap - *pos ? (size_t)n : cap - *pos - 1;
      text_trim_partial_utf8(buf);
      *pos = strlen(buf);
   }
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

static int flashback_add_unique(cJSON *arr, const char *value, size_t max_len)
{
   if (!arr || !value || !value[0] || array_contains_string(arr, value))
      return 0;
   char trimmed[256];
   snprintf(trimmed, sizeof(trimmed), "%.*s", (int)max_len, value);
   text_trim_partial_utf8(trimmed);
   cJSON *item = cJSON_CreateString(trimmed);
   if (!item || !cJSON_AddItemToArray(arr, item))
   {
      cJSON_Delete(item);
      return -1;
   }
   return 0;
}

static int flashback_extract_from_text(cJSON *files, cJSON *errors, cJSON *decisions,
                                       const char *text)
{
   if (!text || !text[0])
      return 0;

   char lower[1024];
   snprintf(lower, sizeof(lower), "%s", text);
   text_trim_partial_utf8(lower);
   for (size_t i = 0; lower[i]; i++)
      if (lower[i] >= 'A' && lower[i] <= 'Z')
         lower[i] = (char)(lower[i] + ('a' - 'A'));

   if (strstr(lower, "error") || strstr(lower, "failed") || strstr(lower, "failure") ||
       strstr(lower, "exception") || strstr(lower, "traceback") || strstr(lower, "denied"))
      if (flashback_add_unique(errors, text, 180))
         return -1;

   if (strstr(lower, "decided") || strstr(lower, "decision") || strstr(lower, "choose") ||
       strstr(lower, "chosen") || strstr(lower, "agreed") || strstr(lower, "will use") ||
       strstr(lower, "use ") || strstr(lower, "using "))
      if (flashback_add_unique(decisions, text, 180))
         return -1;

   char copy[1024];
   snprintf(copy, sizeof(copy), "%s", text);
   text_trim_partial_utf8(copy);
   const char *delims = " \t\r\n,;:()[]{}<>\"'";
   char *save = NULL;
   for (char *tok = strtok_r(copy, delims, &save); tok; tok = strtok_r(NULL, delims, &save))
   {
      if (token_looks_like_path(tok))
         if (flashback_add_unique(files, tok, 160))
            return -1;
   }
   return 0;
}

/* Tool results remain opaque evidence. Scan their literal JSON value rather
 * than interpreting a provider-native content schema inside tool output. */
static int flashback_extract_message(cJSON *files, cJSON *errors, cJSON *decisions,
                                     const aimee_message_t *message)
{
   for (int i = 0; i < message->n_blocks; i++)
   {
      const aimee_block_t *block = &message->blocks[i];
      if (block->type == AIMEE_BLK_TEXT)
      {
         if (flashback_extract_from_text(files, errors, decisions, block->text))
            return -1;
      }
      else if (block->type == AIMEE_BLK_TOOL_RESULT && block->tool_result)
      {
         const char *text = cJSON_GetStringValue(block->tool_result);
         char *json = text ? NULL : cJSON_PrintUnformatted(block->tool_result);
         if (!text && !json)
            return -1;
         int rc = flashback_extract_from_text(files, errors, decisions, text ? text : json);
         free(json);
         if (rc)
            return -1;
      }
   }
   return 0;
}

static cJSON *flashback_build(const compact_history_t *history, int start_idx, int end_idx)
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

   if (!cJSON_AddItemToObject(root, "files_modified", files))
      goto done;
   files = NULL;
   if (!cJSON_AddItemToObject(root, "errors_encountered", errors))
      goto done;
   errors = NULL;
   if (!cJSON_AddItemToObject(root, "decisions_made", decisions))
      goto done;
   decisions = NULL;

   for (int i = start_idx; i < end_idx; i++)
      if (flashback_extract_message(cJSON_GetObjectItem(root, "files_modified"),
                                    cJSON_GetObjectItem(root, "errors_encountered"),
                                    cJSON_GetObjectItem(root, "decisions_made"),
                                    &history->messages[i]))
         goto done;

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

static int append_flashback_json(char *buf, size_t *pos, size_t cap, cJSON *root)
{
   char *json = root ? cJSON_PrintUnformatted(root) : NULL;
   if (!json)
      return -1;
   if (json[0])
      append_format(buf, pos, cap, "Flashback JSON: %s\n\n", json);
   free(json);
   return 0;
}

/* Build a structured text summary of messages[start_idx .. end_idx-1].
 * Writes the result into buf (length SESSION_COMPACT_SUMMARY_MAX). */
static int build_summary(const cJSON *messages, const compact_history_t *history, int start_idx,
                         int end_idx, char *buf, size_t buf_len, int from_record)
{
   if (!buf || buf_len == 0)
      return -1;

   int n = history->count;
   if (start_idx < 0)
      start_idx = 0;
   if (end_idx > n)
      end_idx = n;

   /* We build into a local buffer then copy, to avoid partial writes confusing callers */
   char tmp[SESSION_COMPACT_SUMMARY_MAX];
   size_t pos = 0;

   int n_summarised = end_idx - start_idx;
   char *first_user_text = NULL;
   char *last_user_text = NULL;
   char *last_assistant_text = NULL;
   int tool_call_count = 0;
   int assistant_count = 0;
   char *closet_block = NULL;
   int closet_evict = ECON_MODULE_CLOSET_EVICT_NONE;
   cJSON *flashback = NULL;

   for (int i = start_idx; i < end_idx; i++)
   {
      const aimee_message_t *msg = &history->messages[i];
      const char *role = msg->role;
      if (!strcmp(role, "user") && !msg_is_tool_result(msg))
      {
         char *text = msg_text(msg);
         if (!text)
            goto fail;
         if (text[0])
         {
            if (!first_user_text)
            {
               first_user_text = strdup(text);
               if (!first_user_text)
               {
                  free(text);
                  goto fail;
               }
            }
            free(last_user_text);
            last_user_text = text;
         }
         else
            free(text);
      }
      tool_call_count += msg_tool_count(msg);
      if (!strcmp(role, "assistant"))
      {
         char *text = msg_text(msg);
         if (!text)
            goto fail;
         if (text[0])
         {
            assistant_count++;
            free(last_assistant_text);
            last_assistant_text = text;
         }
         else
            free(text);
      }
   }

   /* The record path falls back to the prose scan if it cannot allocate, so a
    * summary is never emptied by an allocation failure. */
   if (from_record)
      flashback =
          econ_module_record_build(messages, start_idx, end_idx, &closet_block, &closet_evict);
   if (!flashback)
      flashback = flashback_build(history, start_idx, end_idx);
   if (!flashback)
      goto fail;
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

   /* Conserved coordinates: shas, uuids, refs, handles and key=value pairs kept
    * BYTE-EXACT from the discarded turns, so the agent can still address what it
    * can no longer see. A coordinate that did not fit is announced rather than
    * silently dropped — that announcement is the whole point of COORD_EVICT_FAIL. */
   if (closet_block && closet_block[0])
   {
      append_format(tmp, &pos, sizeof(tmp), "## Conserved Coordinates\n");
      append_truncated(tmp, &pos, sizeof(tmp), closet_block, 1600);
      append_format(tmp, &pos, sizeof(tmp), "\n\n");
   }
   if (closet_evict == ECON_MODULE_CLOSET_EVICT_FAIL)
      append_format(tmp, &pos, sizeof(tmp),
                    "- NOTE: some identifiers could not be conserved within the budget; "
                    "re-read the source before relying on any identifier not listed above.\n\n");
   if (append_flashback_json(tmp, &pos, sizeof(tmp), flashback))
      goto fail;
   free(closet_block);
   cJSON_Delete(flashback);
   free(first_user_text);
   free(last_user_text);
   free(last_assistant_text);

   /* Copy to caller's buffer */
   size_t copy_len = pos < buf_len - 1 ? pos : buf_len - 1;
   memcpy(buf, tmp, copy_len);
   buf[copy_len] = '\0';
   text_trim_partial_utf8(buf);
   return 0;
fail:
   free(closet_block);
   cJSON_Delete(flashback);
   free(first_user_text);
   free(last_user_text);
   free(last_assistant_text);
   return -1;
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

/* Capture every owned call before discarding evidence; exact JSON numbers and
 * full identities remain in the canonical source, independent of the metric. */
static int capture_readonly_sigs(const compact_history_t *history, int start, int end,
                                 session_compact_result_t *out)
{
   for (int i = start; i < end; i++)
      for (int j = 0; j < history->messages[i].n_blocks; j++)
      {
         const aimee_block_t *block = &history->messages[i].blocks[j];
         if (block->type != AIMEE_BLK_TOOL_USE)
            continue;
         char *args = block->tool_input ? cJSON_PrintUnformatted(block->tool_input) : NULL;
         if (block->tool_input && !args)
            return -1;
         add_readonly_sig(out, block->tool_name, args ? args : "");
         free(args);
      }
   return 0;
}

/* ------------------------------------------------------------------ public API */

/* Move a canonical compaction cut back to the oldest call answered in the
 * retained suffix. Opaque IDs are compared in full; no fixed-size registry. */
static int canonical_pair_cut(const compact_history_t *history, int cut)
{
   int adjusted;
   do
   {
      adjusted = cut;
      for (int i = cut; i < history->count; i++)
         for (int k = 0; k < history->messages[i].n_blocks; k++)
         {
            const aimee_block_t *result = &history->messages[i].blocks[k];
            if (result->type != AIMEE_BLK_TOOL_RESULT || !result->tool_id)
               continue;
            int found = -1;
            for (int j = i; j >= 0 && found < 0; j--)
               for (int l = 0; l < history->messages[j].n_blocks; l++)
               {
                  const aimee_block_t *call = &history->messages[j].blocks[l];
                  if (call->type == AIMEE_BLK_TOOL_USE && call->tool_id &&
                      !strcmp(call->tool_id, result->tool_id))
                  {
                     found = j;
                     break;
                  }
               }
            if (found >= 0 && found < adjusted)
               adjusted = found;
         }
      if (adjusted == cut)
         return cut;
      cut = adjusted;
   } while (cut > 0);
   return cut;
}

static int canonical_protected(const aimee_message_t *message)
{
   if (!strcmp(message->role, "system") || !strcmp(message->role, "developer"))
      return 1;
   if (strcmp(message->role, "user"))
      return 0;
   for (int i = 0; i < message->n_blocks; i++)
      if (message->blocks[i].type != AIMEE_BLK_TOOL_RESULT)
         return 1;
   return 0;
}

static int append_canonical_summary(cJSON *rebuilt, const cJSON *messages,
                                    const compact_history_t *history, int start, int end,
                                    const session_compact_config_t *cfg)
{
   if (start == end)
      return 0;
   char text[SESSION_COMPACT_SUMMARY_MAX];
   if (build_summary(messages, history, start, end, text, sizeof text, cfg ? cfg->from_record : 0))
      return -1;
   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = text};
   aimee_ir_block_set_context(&block, AIMEE_CTX_ORIGIN_RETRIEVAL, AIMEE_CTX_AUTH_EVIDENCE,
                              AIMEE_CTX_TRUST_UNVERIFIED, AIMEE_CTX_SENS_INTERNAL, 1, NULL, NULL,
                              0);
   aimee_message_t message = {.role = "assistant", .blocks = &block, .n_blocks = 1};
   cJSON *item = aimee_ir_message_json(&message);
   if (!item || !cJSON_AddBoolToObject(item, "_compaction_boundary", 1) ||
       !cJSON_AddItemToArray(rebuilt, item))
   {
      cJSON_Delete(item);
      return -1;
   }
   return 0;
}

/* Canonical compaction retains user/policy messages exactly and folds the
 * evidence between them. Build the complete replacement before changing history. */
static int compact_canonical(cJSON *messages, const compact_history_t *history, int start, int end,
                             const session_compact_config_t *cfg, session_compact_result_t *out)
{
   int n = cJSON_GetArraySize(messages);
   out->messages_after = n;
   for (int i = start; i < end; i++)
   {
      const aimee_message_t *message = &history->messages[i];
      if (canonical_protected(message) && msg_is_tool_result(message))
         return 0; /* A mixed protected result keeps its preceding cycle intact. */
   }
   cJSON *rebuilt = cJSON_CreateArray();
   if (!rebuilt)
      return -1;
   int segment = start;
   for (int i = 0; i < n; i++)
   {
      const cJSON *message = cJSON_GetArrayItem(messages, i);
      int keep = i < start || i >= end || canonical_protected(&history->messages[i]);
      if (i >= start && (keep || i == end))
      {
         if (append_canonical_summary(rebuilt, messages, history, segment, i < end ? i : end, cfg))
            goto fail;
         segment = i + 1;
      }
      if (keep)
      {
         cJSON *copy = cJSON_Duplicate(message, 1);
         if (!copy || !cJSON_AddItemToArray(rebuilt, copy))
         {
            cJSON_Delete(copy);
            goto fail;
         }
      }
      if (i >= end)
         segment = end;
   }
   int after = cJSON_GetArraySize(rebuilt);
   if (after >= n)
   {
      char *before_json = cJSON_PrintUnformatted(messages);
      char *after_json = cJSON_PrintUnformatted(rebuilt);
      int smaller =
          after == n && before_json && after_json && strlen(after_json) < strlen(before_json);
      free(before_json);
      free(after_json);
      if (!smaller)
      {
         cJSON_Delete(rebuilt);
         return 0;
      }
   }
   if (capture_readonly_sigs(history, start, end, out) ||
       build_summary(messages, history, start, end, out->summary, sizeof out->summary,
                     cfg ? cfg->from_record : 0))
      goto fail;
   while (messages->child)
      cJSON_Delete(cJSON_DetachItemViaPointer(messages, messages->child));
   while (rebuilt->child)
      cJSON_AddItemToArray(messages, cJSON_DetachItemViaPointer(rebuilt, rebuilt->child));
   cJSON_Delete(rebuilt);
   out->messages_after = after;
   out->messages_removed = n - after;
   out->compacted = 1;
   return 0;
fail:
   cJSON_Delete(rebuilt);
   return -1;
}

int session_compact(cJSON *messages, const session_compact_config_t *cfg,
                    session_compact_result_t *out)
{
   session_compact_result_t result = {0};
   compact_history_t history = {0};
   cJSON *working = NULL;
   int rc = -1;
   if (aimee_ir_history_admit_host(messages) || history_load(messages, &history))
      goto done;
   int n = history.count;
   result.messages_before = result.messages_after = n;
   int retain = resolve_retain_tail(cfg);
   if (n < 2 || retain >= n - 1)
   {
      rc = 0;
      goto done;
   }
   /* Summaries are staged after strict causal admission. Any refusal leaves
    * source bytes, opaque values and cache metadata unchanged. */
   working = cJSON_Duplicate(messages, 1);
   if (!working)
      goto done;
   int start = 1, end = canonical_pair_cut(&history, n - retain);
   if (msg_tool_count(&history.messages[0]))
      while (start < end && msg_is_tool_result(&history.messages[start]))
         start++;
   if (end > start && compact_canonical(working, &history, start, end, cfg, &result))
      goto done;
   if (aimee_ir_history_admit_host(working))
      goto done;
   result.messages_after = cJSON_GetArraySize(working);
   while (messages->child)
      cJSON_Delete(cJSON_DetachItemViaPointer(messages, messages->child));
   while (working->child)
      cJSON_AddItemToArray(messages, cJSON_DetachItemViaPointer(working, working->child));
   rc = 0;
done:
   history_free(&history);
   cJSON_Delete(working);
   if (rc)
      memset(&result, 0, sizeof result);
   if (out)
      *out = result;
   return rc;
}

int session_compact_focused(const char *topic, char *out, size_t out_len)
{
   if (!topic || !out || out_len == 0)
      return -1;

   aimee_block_t block = {.type = AIMEE_BLK_TEXT, .text = (char *)topic};
   aimee_message_t message = {.role = "user", .blocks = &block, .n_blocks = 1};
   compact_history_t history = {.messages = &message, .count = 1};
   cJSON *messages = cJSON_CreateArray();
   cJSON *item = aimee_ir_message_json(&message);
   if (!messages || !item || !cJSON_AddItemToArray(messages, item))
   {
      cJSON_Delete(messages);
      cJSON_Delete(item);
      return -1;
   }
   int rc = build_summary(messages, &history, 0, 1, out, out_len, 0);
   cJSON_Delete(messages);
   return rc;
}
