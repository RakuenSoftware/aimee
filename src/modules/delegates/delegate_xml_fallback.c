/* delegate_xml_fallback.c: XML-style tool-call parser for models without native tool calling.
 *
 * Some weaker or older models return tool calls as XML in their response text:
 *
 *   <tool_call>
 *     <name>bash</name>
 *     <arguments>{"command": "ls -la"}</arguments>
 *   </tool_call>
 *
 * This module parses such responses and fills a parsed_response_t.
 */
#include "aimee.h"
#include "delegate_xml_fallback.h"
#include "agent_tools.h"
#include "cJSON.h"
#include <string.h>
#include <ctype.h>

/* Find the first occurrence of <tag> in haystack.
 * Returns pointer to the start of the content inside the tag, or NULL.
 * Sets *end_out to the closing </tag> position if found. */
static const char *find_xml_tag(const char *haystack, const char *tag, const char **end_out)
{
   char open[64], close[64];
   snprintf(open, sizeof(open), "<%s>", tag);
   snprintf(close, sizeof(close), "</%s>", tag);

   const char *start = strstr(haystack, open);
   if (!start)
      return NULL;
   start += strlen(open);

   const char *end = strstr(start, close);
   if (!end)
      return NULL;

   if (end_out)
      *end_out = end;
   return start;
}

static const char *find_balanced_json_end(const char *open);

static const char *find_channel_tool_end(const char *args_open)
{
   const char *end = find_balanced_json_end(args_open);
   return end ? end - 1 : NULL;
}

/* Trim leading/trailing whitespace in-place (modifies buf). */
static void trim_inplace(char *buf)
{
   if (!buf || !buf[0])
      return;
   /* Leading */
   size_t start = 0;
   while (buf[start] && isspace((unsigned char)buf[start]))
      start++;
   if (start > 0)
      memmove(buf, buf + start, strlen(buf + start) + 1);
   /* Trailing */
   size_t len = strlen(buf);
   while (len > 0 && isspace((unsigned char)buf[len - 1]))
      buf[--len] = '\0';
}

static const char *find_ci(const char *haystack, const char *needle)
{
   size_t nlen;
   if (!haystack || !needle || !needle[0])
      return NULL;
   nlen = strlen(needle);
   for (const char *p = haystack; *p; p++)
   {
      size_t i = 0;
      while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
         i++;
      if (i == nlen)
         return p;
   }
   return NULL;
}

static void remove_reasoning_block(char *buf, const char *open_tag, const char *close_tag)
{
   if (!buf || !open_tag || !close_tag)
      return;

   size_t close_len = strlen(close_tag);
   char *open = NULL;
   while ((open = (char *)find_ci(buf, open_tag)) != NULL)
   {
      char *close = (char *)find_ci(open + strlen(open_tag), close_tag);
      char *tail = close ? close + close_len : open + strlen(open);
      memmove(open, tail, strlen(tail) + 1);
   }
}

static char *strip_reasoning_blocks(const char *text)
{
   char *buf;
   if (!text)
      return NULL;
   buf = strdup(text);
   if (!buf)
      return NULL;
   remove_reasoning_block(buf, "<think>", "</think>");
   remove_reasoning_block(buf, "[THINK]", "[/THINK]");
   trim_inplace(buf);
   return buf;
}

static char *copy_trimmed(const char *start, size_t len)
{
   char *buf = malloc(len + 1);
   if (!buf)
      return NULL;
   memcpy(buf, start, len);
   buf[len] = '\0';
   trim_inplace(buf);
   return buf;
}

static const char *find_within(const char *start, const char *end, const char *needle)
{
   size_t nlen = strlen(needle);
   if (!start || !end || start >= end || nlen == 0)
      return NULL;
   for (const char *p = start; p + nlen <= end; p++)
   {
      if (strncmp(p, needle, nlen) == 0)
         return p;
   }
   return NULL;
}

static const char *find_local_xml_tag(const char *haystack, const char *local_name,
                                      const char **end_out, size_t *close_len_out)
{
   if (!haystack || !local_name || !local_name[0])
      return NULL;

   for (const char *p = haystack; (p = strchr(p, '<')) != NULL; p++)
   {
      if (p[1] == '/' || p[1] == '!' || p[1] == '?')
         continue;

      const char *tag_start = p + 1;
      const char *tag_end = tag_start;
      while (*tag_end && *tag_end != '>' && !isspace((unsigned char)*tag_end))
         tag_end++;
      if (*tag_end == '\0')
         return NULL;

      const char *local_start = tag_start;
      for (const char *q = tag_start; q < tag_end; q++)
      {
         if (*q == ':')
            local_start = q + 1;
      }
      if ((size_t)(tag_end - local_start) != strlen(local_name) ||
          strncmp(local_start, local_name, strlen(local_name)) != 0)
         continue;

      const char *open_end = strchr(tag_end, '>');
      if (!open_end)
         return NULL;

      size_t tag_len = (size_t)(tag_end - tag_start);
      char close[96];
      if (tag_len + 4 > sizeof(close))
         return NULL;
      snprintf(close, sizeof(close), "</%.*s>", (int)tag_len, tag_start);
      const char *close_start = strstr(open_end + 1, close);
      if (!close_start)
         continue;

      if (end_out)
         *end_out = close_start;
      if (close_len_out)
         *close_len_out = strlen(close);
      return open_end + 1;
   }

   return NULL;
}

static char *xml_attr_value(const char *tag_start, const char *tag_end, const char *attr)
{
   if (!tag_start || !tag_end || !attr)
      return NULL;

   size_t attr_len = strlen(attr);
   const char *p = tag_start;
   while (p && p < tag_end)
   {
      p = find_within(p, tag_end, attr);
      if (!p)
         return NULL;
      if ((p == tag_start || isspace((unsigned char)p[-1])) && p + attr_len < tag_end &&
          p[attr_len] == '=')
         break;
      p += attr_len;
   }
   if (!p || p + attr_len >= tag_end || p[attr_len] != '=')
      return NULL;

   p += attr_len + 1;
   while (p < tag_end && isspace((unsigned char)*p))
      p++;
   if (p >= tag_end || (*p != '"' && *p != '\''))
      return NULL;

   char quote = *p++;
   const char *value_start = p;
   while (p < tag_end && *p != quote)
      p++;
   if (p >= tag_end)
      return NULL;

   return copy_trimmed(value_start, (size_t)(p - value_start));
}

static const char *find_json_object_end(const char *open)
{
   if (!open || *open != '{')
      return NULL;

   int depth = 0;
   int in_string = 0;
   int escaped = 0;
   for (const char *p = open; *p; p++)
   {
      char ch = *p;
      if (in_string)
      {
         if (escaped)
            escaped = 0;
         else if (ch == '\\')
            escaped = 1;
         else if (ch == '"')
            in_string = 0;
         continue;
      }

      if (ch == '"')
      {
         in_string = 1;
      }
      else if (ch == '{')
      {
         depth++;
      }
      else if (ch == '}')
      {
         depth--;
         if (depth == 0)
            return p;
      }
   }
   return NULL;
}

static int tool_name_known_for_rescue(const char *name)
{
   return name && name[0] &&
          (strcmp(name, "respond") == 0 || strchr(name, ':') != NULL ||
           agent_tool_get_schema_cached(name));
}

static void normalize_tool_name(char *name)
{
   if (!name || !name[0])
      return;
   if (strcmp(name, "Bash") == 0)
   {
      strcpy(name, "bash");
      return;
   }
   for (char *p = name; *p; p++)
   {
      if (*p == '-')
         *p = '_';
      else
         *p = (char)tolower((unsigned char)*p);
   }
}

static void fill_tool_call(parsed_tool_call_t *tc, int call_index, const char *name,
                           size_t name_len, const char *args_start, size_t args_len)
{
   memset(tc, 0, sizeof(*tc));
   snprintf(tc->id, sizeof(tc->id), "xml_call_%d", call_index);

   if (name_len >= sizeof(tc->name))
      name_len = sizeof(tc->name) - 1;
   memcpy(tc->name, name, name_len);
   tc->name[name_len] = '\0';
   trim_inplace(tc->name);
   normalize_tool_name(tc->name);

   if (args_start && args_len > 0)
      tc->arguments = copy_trimmed(args_start, args_len);
   if (!tc->arguments || !tc->arguments[0])
   {
      free(tc->arguments);
      tc->arguments = strdup("{}");
   }
}

static void add_parameter_value(cJSON *obj, const char *key, char *value)
{
   if (!obj || !key || !key[0] || !value)
      return;

   cJSON *parsed = cJSON_Parse(value);
   if (parsed)
   {
      cJSON_AddItemToObject(obj, key, parsed);
      return;
   }

   cJSON_AddStringToObject(obj, key, value);
}

static int parse_qwen_function_tool_call(const char *block_start, const char *block_end,
                                         parsed_response_t *out)
{
   const char *fn = find_within(block_start, block_end, "<function=");
   if (!fn)
      return 0;

   const char *name_start = fn + strlen("<function=");
   const char *name_end = strchr(name_start, '>');
   if (!name_end || name_end > block_end)
      return 0;

   parsed_tool_call_t *tc = &out->calls[out->call_count++];
   memset(tc, 0, sizeof(*tc));

   size_t name_len = (size_t)(name_end - name_start);
   if (name_len >= sizeof(tc->name))
      name_len = sizeof(tc->name) - 1;
   memcpy(tc->name, name_start, name_len);
   tc->name[name_len] = '\0';
   trim_inplace(tc->name);
   normalize_tool_name(tc->name);
   snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);

   cJSON *args = cJSON_CreateObject();
   if (!args)
   {
      tc->arguments = strdup("{}");
      return 1;
   }

   const char *body_start = name_end + 1;
   const char *body_end = find_within(body_start, block_end, "</function>");
   if (!body_end)
      body_end = block_end;

   const char *p = body_start;
   while (p < body_end)
   {
      const char *param = find_within(p, body_end, "<parameter=");
      if (!param)
         break;
      const char *key_start = param + strlen("<parameter=");
      const char *key_end = strchr(key_start, '>');
      if (!key_end || key_end > body_end)
         break;

      const char *value_start = key_end + 1;
      const char *value_end = find_within(value_start, body_end, "</parameter>");
      if (!value_end)
         break;

      char *key = copy_trimmed(key_start, (size_t)(key_end - key_start));
      char *value = copy_trimmed(value_start, (size_t)(value_end - value_start));
      if (key && key[0] && value)
         add_parameter_value(args, key, value);
      free(key);
      free(value);
      p = value_end + strlen("</parameter>");
   }

   tc->arguments = cJSON_PrintUnformatted(args);
   cJSON_Delete(args);
   if (!tc->arguments)
      tc->arguments = strdup("{}");
   return 1;
}

static int parse_invoke_tool_call(const char *block_start, const char *block_end,
                                  parsed_response_t *out)
{
   const char *invoke = find_within(block_start, block_end, "<invoke");
   if (!invoke)
      return 0;

   const char *invoke_tag_end = find_within(invoke, block_end, ">");
   if (!invoke_tag_end)
      return 0;

   char *name = xml_attr_value(invoke, invoke_tag_end, "name");
   if (!name || !name[0])
   {
      free(name);
      return 0;
   }
   normalize_tool_name(name);
   if (!tool_name_known_for_rescue(name))
   {
      free(name);
      return 0;
   }

   const char *body_start = invoke_tag_end + 1;
   const char *body_end = find_within(body_start, block_end, "</invoke>");
   if (!body_end)
      body_end = block_end;

   cJSON *args = cJSON_CreateObject();
   if (!args)
   {
      free(name);
      return 0;
   }

   const char *p = body_start;
   while (p < body_end)
   {
      const char *param = find_within(p, body_end, "<parameter");
      if (!param)
         break;
      const char *tag_end = find_within(param, body_end, ">");
      if (!tag_end)
         break;

      char *key = xml_attr_value(param, tag_end, "name");
      const char *value_start = tag_end + 1;
      const char *value_end = find_within(value_start, body_end, "</parameter>");
      if (!value_end)
      {
         free(key);
         break;
      }

      char *value = copy_trimmed(value_start, (size_t)(value_end - value_start));
      if (key && key[0] && value)
         add_parameter_value(args, key, value);
      free(key);
      free(value);
      p = value_end + strlen("</parameter>");
   }

   parsed_tool_call_t *tc = &out->calls[out->call_count++];
   memset(tc, 0, sizeof(*tc));
   snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);
   snprintf(tc->name, sizeof(tc->name), "%s", name);
   tc->arguments = cJSON_PrintUnformatted(args);
   if (!tc->arguments)
      tc->arguments = strdup("{}");

   cJSON_Delete(args);
   free(name);
   return 1;
}

static int parse_invoke_tool_calls(const char *text, parsed_response_t *out)
{
   int found = 0;
   const char *p = text;

   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      const char *invoke = strstr(p, "<invoke");
      if (!invoke)
         break;

      const char *tag_end = strchr(invoke, '>');
      if (!tag_end)
         break;

      const char *close = strstr(tag_end + 1, "</invoke>");
      const char *block_end = close ? close + strlen("</invoke>") : tag_end + 1;
      int before = out->call_count;
      if (parse_invoke_tool_call(invoke, block_end, out))
      {
         found++;
         if (!out->content && invoke > text)
         {
            char *pre = copy_trimmed(text, (size_t)(invoke - text));
            if (pre && pre[0])
               out->content = pre;
            else
               free(pre);
         }
      }

      if (out->call_count == before)
         p = tag_end + 1;
      else
         p = block_end;
   }

   if (found > 0)
      out->is_tool_call = 1;
   return found;
}

static char *decode_channel_arg_text(const char *start, size_t len)
{
   static const char quote_marker[] = "<|\"|>";
   char *out = malloc(len + 1);
   if (!out)
      return NULL;
   size_t j = 0;
   for (size_t i = 0; i < len;)
   {
      if (i + sizeof(quote_marker) - 1 <= len &&
          strncmp(start + i, quote_marker, sizeof(quote_marker) - 1) == 0)
      {
         out[j++] = '"';
         i += sizeof(quote_marker) - 1;
      }
      else
      {
         out[j++] = start[i++];
      }
   }
   out[j] = '\0';
   trim_inplace(out);
   return out;
}

static char *channel_args_to_json(const char *args_start, size_t args_len)
{
   char *decoded = decode_channel_arg_text(args_start, args_len);
   if (!decoded)
      return strdup("{}");

   cJSON *check = cJSON_Parse(decoded);
   if (check)
   {
      cJSON_Delete(check);
      return decoded;
   }

   char *colon = strchr(decoded, ':');
   cJSON *obj = cJSON_CreateObject();
   if (!obj)
   {
      free(decoded);
      return strdup("{}");
   }

   if (colon)
   {
      *colon = '\0';
      char *key = decoded;
      char *value = colon + 1;
      trim_inplace(key);
      trim_inplace(value);
      size_t value_len = strlen(value);
      if (value_len >= 2 && value[0] == '"' && value[value_len - 1] == '"')
      {
         value[value_len - 1] = '\0';
         value++;
      }
      cJSON_AddStringToObject(obj, key[0] ? key : "value", value);
   }
   else
   {
      cJSON_AddStringToObject(obj, "value", decoded);
   }

   char *json = cJSON_PrintUnformatted(obj);
   cJSON_Delete(obj);
   free(decoded);
   return json ? json : strdup("{}");
}

static int parse_channel_tool_calls(const char *text, parsed_response_t *out)
{
   static const char marker[] = "<|channel>call:";
   int found = 0;
   const char *p = text;

   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      const char *start = strstr(p, marker);
      if (!start)
         break;
      const char *name_start = start + sizeof(marker) - 1;
      while (*name_start && isspace((unsigned char)*name_start))
         name_start++;
      const char *name_end = name_start;
      while (*name_end && *name_end != '{' && !isspace((unsigned char)*name_end) &&
             *name_end != '<')
         name_end++;

      const char *args_open = strchr(name_end, '{');
      if (!args_open)
         break;
      const char *args_close = find_channel_tool_end(args_open);
      if (!args_close)
         break;

      parsed_tool_call_t *tc = &out->calls[out->call_count++];
      memset(tc, 0, sizeof(*tc));
      snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);

      size_t name_len = (size_t)(name_end - name_start);
      if (name_len >= sizeof(tc->name))
         name_len = sizeof(tc->name) - 1;
      memcpy(tc->name, name_start, name_len);
      tc->name[name_len] = '\0';
      trim_inplace(tc->name);
      normalize_tool_name(tc->name);

      tc->arguments = channel_args_to_json(args_open + 1, (size_t)(args_close - args_open - 1));
      found++;
      p = args_close + 1;
   }

   if (found > 0)
   {
      out->is_tool_call = 1;
      const char *first = strstr(text, marker);
      if (first && first > text)
      {
         size_t pre_len = (size_t)(first - text);
         char *pre = malloc(pre_len + 1);
         if (pre)
         {
            memcpy(pre, text, pre_len);
            pre[pre_len] = '\0';
            trim_inplace(pre);
            if (pre[0])
               out->content = pre;
            else
               free(pre);
         }
      }
   }

   return found;
}

static const char *find_balanced_json_end(const char *open)
{
   const char *close = find_json_object_end(open);
   return close ? close + 1 : NULL;
}

static int parse_mistral_bracket_tool_calls(const char *text, parsed_response_t *out)
{
   static const char marker[] = "[TOOL_CALLS]";
   int found = 0;
   const char *p = text;

   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      const char *start = strstr(p, marker);
      if (!start)
         break;
      const char *name_start = start + sizeof(marker) - 1;
      while (*name_start && (isspace((unsigned char)*name_start) || *name_start == ','))
         name_start++;

      const char *name_end = name_start;
      while (*name_end && (isalnum((unsigned char)*name_end) || *name_end == '_' ||
                           *name_end == '-' || *name_end == '.' || *name_end == ':'))
         name_end++;
      if (name_end == name_start)
         break;

      const char *args_open = name_end;
      while (*args_open && isspace((unsigned char)*args_open))
         args_open++;
      if (*args_open != '{')
         break;
      const char *args_close = find_json_object_end(args_open);
      if (!args_close)
         break;
      char name_buf[sizeof(out->calls[0].name)];
      size_t name_len = (size_t)(name_end - name_start);
      if (name_len >= sizeof(name_buf))
         name_len = sizeof(name_buf) - 1;
      memcpy(name_buf, name_start, name_len);
      name_buf[name_len] = '\0';
      trim_inplace(name_buf);
      if (!tool_name_known_for_rescue(name_buf))
      {
         p = args_close + 1;
         continue;
      }

      fill_tool_call(&out->calls[out->call_count], out->call_count + 1, name_buf, strlen(name_buf),
                     args_open, (size_t)(args_close - args_open + 1));
      out->call_count++;
      found++;
      p = args_close + 1;
   }

   if (found > 0)
   {
      out->is_tool_call = 1;
      const char *first = strstr(text, marker);
      if (first && first > text)
      {
         char *pre = copy_trimmed(text, (size_t)(first - text));
         if (pre && pre[0])
            out->content = pre;
         else
            free(pre);
      }
   }
   return found;
}

static char *json_arguments_from_item(cJSON *item)
{
   if (!item)
      return strdup("{}");
   if (cJSON_IsObject(item))
   {
      char *printed = cJSON_PrintUnformatted(item);
      return printed ? printed : strdup("{}");
   }
   if (cJSON_IsString(item) && item->valuestring)
   {
      cJSON *parsed = cJSON_Parse(item->valuestring);
      if (parsed && cJSON_IsObject(parsed))
      {
         char *printed = cJSON_PrintUnformatted(parsed);
         cJSON_Delete(parsed);
         return printed ? printed : strdup("{}");
      }
      if (parsed)
         cJSON_Delete(parsed);
   }
   return strdup("{}");
}

static int parse_json_tool_object(const char *start, size_t len, parsed_response_t *out)
{
   if (!out || out->call_count >= AGENT_MAX_TOOL_CALLS)
      return 0;

   char *json = copy_trimmed(start, len);
   if (!json)
      return 0;

   cJSON *root = cJSON_Parse(json);
   free(json);
   if (!root || !cJSON_IsObject(root))
   {
      cJSON_Delete(root);
      return 0;
   }

   cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "tool");
   if (!cJSON_IsString(jname))
      jname = cJSON_GetObjectItemCaseSensitive(root, "name");
   if (!cJSON_IsString(jname) || !tool_name_known_for_rescue(jname->valuestring))
   {
      cJSON_Delete(root);
      return 0;
   }

   cJSON *jargs = cJSON_GetObjectItemCaseSensitive(root, "args");
   if (!jargs)
      jargs = cJSON_GetObjectItemCaseSensitive(root, "arguments");

   parsed_tool_call_t *tc = &out->calls[out->call_count++];
   memset(tc, 0, sizeof(*tc));
   snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);
   snprintf(tc->name, sizeof(tc->name), "%s", jname->valuestring);
   tc->arguments = json_arguments_from_item(jargs);
   cJSON_Delete(root);
   return 1;
}

static int parse_bare_json_tool_calls(const char *text, parsed_response_t *out)
{
   int found = 0;
   const char *p = text;
   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      const char *open = strchr(p, '{');
      if (!open)
         break;
      const char *close = find_json_object_end(open);
      if (!close)
         break;
      int before = out->call_count;
      if (parse_json_tool_object(open, (size_t)(close - open + 1), out))
      {
         found++;
         if (!out->content && open > text)
         {
            char *pre = copy_trimmed(text, (size_t)(open - text));
            if (pre && pre[0])
               out->content = pre;
            else
               free(pre);
         }
      }
      if (out->call_count == before)
         p = open + 1;
      else
         p = close + 1;
   }

   if (found > 0)
      out->is_tool_call = 1;
   return found;
}

int delegate_rescue_parse_tool_calls(const char *text, parsed_response_t *out, int allow_json)
{
   if (!text || !out)
      return 0;

   char *stripped = strip_reasoning_blocks(text);
   const char *scan = stripped ? stripped : text;

   int found = 0;
   const char *p = scan;

   while (out->call_count < AGENT_MAX_TOOL_CALLS)
   {
      /* Look for <tool_call> block */
      const char *block_end;
      const char *block_start = find_xml_tag(p, "tool_call", &block_end);
      if (!block_start)
         break;

      /* Extract name */
      const char *name_end;
      const char *name_start = find_xml_tag(block_start, "name", &name_end);

      /* Extract arguments */
      const char *args_end;
      const char *args_start = find_xml_tag(block_start, "arguments", &args_end);

      /* find_xml_tag scans the whole remaining string, so a <name>/<arguments>
       * that actually belongs to a LATER <tool_call> block (i.e. closes past
       * this block's </tool_call>) must not be attributed to this block — that
       * would fabricate a tool call mixing one block's name with another's
       * arguments. Bound both to the current block. */
      if (name_end && name_end > block_end)
      {
         name_start = NULL;
         name_end = NULL;
      }
      if (args_end && args_end > block_end)
      {
         args_start = NULL;
         args_end = NULL;
      }

      if (name_start && name_end)
      {
         parsed_tool_call_t *tc = &out->calls[out->call_count++];
         memset(tc, 0, sizeof(*tc));
         snprintf(tc->id, sizeof(tc->id), "xml_call_%d", out->call_count);

         /* Copy name */
         size_t name_len = (size_t)(name_end - name_start);
         if (name_len >= sizeof(tc->name))
            name_len = sizeof(tc->name) - 1;
         memcpy(tc->name, name_start, name_len);
         tc->name[name_len] = '\0';
         trim_inplace(tc->name);
         normalize_tool_name(tc->name);

         /* Copy arguments */
         if (args_start && args_end)
         {
            size_t args_len = (size_t)(args_end - args_start);
            char *args_buf = malloc(args_len + 1);
            if (args_buf)
            {
               memcpy(args_buf, args_start, args_len);
               args_buf[args_len] = '\0';
               trim_inplace(args_buf);

               /* Validate JSON; fall back to wrapping in {"command":"..."} if not valid */
               cJSON *check = cJSON_Parse(args_buf);
               if (check)
               {
                  tc->arguments = args_buf;
                  cJSON_Delete(check);
               }
               else
               {
                  /* Not valid JSON: wrap as a string value */
                  cJSON *wrapped = cJSON_CreateObject();
                  cJSON_AddStringToObject(wrapped, "value", args_buf);
                  tc->arguments = cJSON_PrintUnformatted(wrapped);
                  cJSON_Delete(wrapped);
                  free(args_buf);
               }
            }
            else
            {
               tc->arguments = strdup("{}");
            }
         }
         else
         {
            tc->arguments = strdup("{}");
         }

         found++;
      }
      else if (parse_qwen_function_tool_call(block_start, block_end, out))
      {
         found++;
      }

      /* Advance past this <tool_call> block */
      p = block_end + strlen("</tool_call>");
      if (p > scan + strlen(scan))
         break;
   }

   if (found == 0)
   {
      p = scan;
      while (out->call_count < AGENT_MAX_TOOL_CALLS)
      {
         const char *block_end;
         size_t close_len = 0;
         const char *block_start = find_local_xml_tag(p, "tool_call", &block_end, &close_len);
         if (!block_start)
            break;

         if (parse_invoke_tool_call(block_start, block_end, out))
            found++;
         p = block_end + close_len;
      }
   }

   if (found > 0)
   {
      out->is_tool_call = 1;
      /* Store any text before the first tool call as content */
      const char *first_block = strstr(scan, "<tool_call>");
      if (!first_block)
      {
         const char *ns = strstr(scan, ":tool_call>");
         if (ns)
         {
            while (ns > scan && *ns != '<')
               ns--;
            if (*ns == '<')
               first_block = ns;
         }
      }
      if (first_block && first_block > scan)
      {
         size_t pre_len = (size_t)(first_block - scan);
         char *pre = malloc(pre_len + 1);
         if (pre)
         {
            memcpy(pre, scan, pre_len);
            pre[pre_len] = '\0';
            trim_inplace(pre);
            if (pre[0])
               out->content = pre;
            else
               free(pre);
         }
      }
   }

   if (found == 0)
      found = parse_channel_tool_calls(scan, out);
   if (found == 0)
      found = parse_invoke_tool_calls(scan, out);
   if (found == 0)
      found = parse_mistral_bracket_tool_calls(scan, out);
   if (found == 0 && allow_json)
      found = parse_bare_json_tool_calls(scan, out);

   free(stripped);
   return found;
}

int xml_parse_tool_calls(const char *text, parsed_response_t *out)
{
   return delegate_rescue_parse_tool_calls(text, out, 1);
}

int delegate_rescue_has_tool_calls_with_json(const char *text, int allow_json)
{
   if (!text)
      return 0;
   if (strstr(text, "<tool_call>") != NULL || strstr(text, "<invoke") != NULL ||
       strstr(text, "<|channel>call:") != NULL || strstr(text, "[TOOL_CALLS]") != NULL ||
       strstr(text, ":tool_call>") != NULL)
      return 1;

   if (!allow_json)
      return 0;

   char *stripped = strip_reasoning_blocks(text);
   const char *scan = stripped ? stripped : text;
   int has_json = 0;
   const char *p = scan;
   while (!has_json)
   {
      const char *open = strchr(p, '{');
      if (!open)
         break;
      const char *close = find_json_object_end(open);
      if (!close)
         break;
      parsed_response_t tmp;
      memset(&tmp, 0, sizeof(tmp));
      has_json = parse_json_tool_object(open, (size_t)(close - open + 1), &tmp);
      agent_free_parsed_response(&tmp);
      p = open + 1;
   }
   free(stripped);
   return has_json;
}

int xml_has_tool_calls(const char *text)
{
   return delegate_rescue_has_tool_calls(text);
}

int delegate_rescue_has_tool_calls(const char *text)
{
   return delegate_rescue_has_tool_calls_with_json(text, 1);
}
