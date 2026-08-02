/* computer_use.c: deterministic guard policy for external GUI/browser tools. */
#include "computer_use.h"

#include "cJSON.h"
#include "config_accessors.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

static int contains_ci(const char *s, const char *needle)
{
   if (!s || !needle || !needle[0])
      return 0;
   size_t n = strlen(needle);
   for (const char *p = s; *p; p++)
   {
      size_t i = 0;
      for (; i < n && p[i]; i++)
      {
         if (tolower((unsigned char)p[i]) != tolower((unsigned char)needle[i]))
            break;
      }
      if (i == n)
         return 1;
   }
   return 0;
}

static const char *json_string_any(cJSON *root, const char *a, const char *b, const char *c)
{
   cJSON *v = root && a ? cJSON_GetObjectItemCaseSensitive(root, a) : NULL;
   if (!cJSON_IsString(v) && b)
      v = cJSON_GetObjectItemCaseSensitive(root, b);
   if (!cJSON_IsString(v) && c)
      v = cJSON_GetObjectItemCaseSensitive(root, c);
   return cJSON_IsString(v) ? v->valuestring : "";
}

static const char *tool_action(const char *qualified)
{
   const char *sep = qualified ? strchr(qualified, ':') : NULL;
   return sep && sep[1] ? sep + 1 : "";
}

int computer_use_is_tool_name(const char *qualified_tool_name)
{
   if (!qualified_tool_name)
      return 0;
   return strncmp(qualified_tool_name, "computer_use:", 13) == 0 ||
          strncmp(qualified_tool_name, "computer-use:", 13) == 0;
}

void computer_use_policy_from_config(computer_use_policy_t *out)
{
   if (!out)
      return;
   memset(out, 0, sizeof(*out));
   snprintf(out->default_navigation, sizeof(out->default_navigation), "%s", "approve");
   out->redact_sensitive_screenshots = 1;
   snprintf(out->allowed_domains[0], sizeof(out->allowed_domains[0]), "%s", "localhost");
   out->allowed_domain_count = 1;

   out->enabled = config_computer_use_enabled();
   const char *nav = config_computer_use_default_navigation();
   if (nav && nav[0])
      snprintf(out->default_navigation, sizeof(out->default_navigation), "%s", nav);
   out->redact_sensitive_screenshots = config_computer_use_redact_sensitive_screenshots();
   int domain_count = config_computer_use_allowed_domain_count();
   if (domain_count > 0)
   {
      out->allowed_domain_count = domain_count;
      if (out->allowed_domain_count > COMPUTER_USE_MAX_ALLOWED_DOMAINS)
         out->allowed_domain_count = COMPUTER_USE_MAX_ALLOWED_DOMAINS;
      for (int i = 0; i < out->allowed_domain_count; i++)
         snprintf(out->allowed_domains[i], sizeof(out->allowed_domains[i]), "%s",
                  config_computer_use_allowed_domains(i));
   }
}

static void url_host(const char *url, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return;
   out[0] = '\0';
   if (!url)
      return;
   const char *p = strstr(url, "://");
   p = p ? p + 3 : url;
   const char *start = p;
   while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#')
      p++;
   snprintf(out, out_len, "%.*s", (int)(p - start), start);
   for (char *q = out; *q; q++)
      *q = (char)tolower((unsigned char)*q);
}

static int domain_allowed(const computer_use_policy_t *policy, const char *host)
{
   if (!policy || !host || !host[0])
      return 0;
   if (strcmp(host, "localhost") == 0 || strcmp(host, "127.0.0.1") == 0 || strcmp(host, "::1") == 0)
      return 1;
   for (int i = 0; i < policy->allowed_domain_count; i++)
   {
      const char *pat = policy->allowed_domains[i];
      if (!pat[0])
         continue;
      if (strcmp(pat, host) == 0)
         return 1;
      if (strncmp(pat, "*.", 2) == 0)
      {
         const char *suffix = pat + 1;
         size_t hlen = strlen(host), slen = strlen(suffix);
         if (hlen >= slen && strcmp(host + hlen - slen, suffix) == 0)
            return 1;
      }
   }
   return 0;
}

static int sensitive_text(const char *s)
{
   return contains_ci(s, "password") || contains_ci(s, "login") || contains_ci(s, "sign in") ||
          contains_ci(s, "payment") || contains_ci(s, "pay ") || contains_ci(s, "credit card") ||
          contains_ci(s, "cvv") || contains_ci(s, "token") || contains_ci(s, "secret") ||
          contains_ci(s, "upload") || contains_ci(s, "download");
}

int computer_use_classify(const computer_use_policy_t *policy, const char *qualified_tool_name,
                          const char *args_json, computer_use_decision_t *decision_out,
                          char *reason_out, size_t reason_len)
{
   if (decision_out)
      *decision_out = COMPUTER_USE_DECISION_ALLOW;
   if (reason_out && reason_len > 0)
      reason_out[0] = '\0';
   if (!computer_use_is_tool_name(qualified_tool_name))
      return 0;
   if (!policy || !policy->enabled)
   {
      if (decision_out)
         *decision_out = COMPUTER_USE_DECISION_BLOCK;
      if (reason_out && reason_len > 0)
         snprintf(reason_out, reason_len, "computer-use tools are disabled");
      return 1;
   }

   cJSON *args = args_json ? cJSON_Parse(args_json) : NULL;
   const char *action = tool_action(qualified_tool_name);
   computer_use_decision_t decision = COMPUTER_USE_DECISION_ALLOW;
   const char *reason = "computer-use observe/interact action allowed";

   if (contains_ci(action, "open") || contains_ci(action, "navigate") || contains_ci(action, "url"))
   {
      char host[256];
      url_host(json_string_any(args, "url", "target", "href"), host, sizeof(host));
      if (domain_allowed(policy, host))
         reason = "navigation target is allowlisted";
      else if (strcmp(policy->default_navigation, "allow") == 0)
         reason = "off-allowlist navigation allowed by policy";
      else
      {
         decision = strcmp(policy->default_navigation, "block") == 0
                        ? COMPUTER_USE_DECISION_BLOCK
                        : COMPUTER_USE_DECISION_APPROVE;
         reason = "off-allowlist navigation requires approval";
      }
   }
   else if (contains_ci(action, "type") || contains_ci(action, "click"))
   {
      const char *field = json_string_any(args, "field_type", "selector", "label");
      const char *text = json_string_any(args, "text", "value", "target");
      if (sensitive_text(field) || sensitive_text(text))
      {
         decision = COMPUTER_USE_DECISION_APPROVE;
         reason = "sensitive GUI interaction requires approval";
      }
   }
   else if (contains_ci(action, "screenshot"))
   {
      const char *page_class = json_string_any(args, "page_class", "classification", "label");
      if (sensitive_text(page_class))
      {
         decision = COMPUTER_USE_DECISION_APPROVE;
         reason = "sensitive screenshot requires approval";
      }
      else
         reason = "screenshot observation allowed";
   }
   else if (contains_ci(action, "upload") || contains_ci(action, "download"))
   {
      decision = COMPUTER_USE_DECISION_APPROVE;
      reason = "file transfer through GUI requires approval";
   }

   if (decision_out)
      *decision_out = decision;
   if (reason_out && reason_len > 0)
      snprintf(reason_out, reason_len, "%s", reason);
   cJSON_Delete(args);
   return 1;
}

static void append_redacted(char *out, size_t out_len, size_t *pos, const char *text)
{
   if (!out || !pos || *pos >= out_len)
      return;
   int n = snprintf(out + *pos, out_len - *pos, "%s", text);
   if (n > 0)
      *pos += (size_t)n < out_len - *pos ? (size_t)n : out_len - *pos - 1;
}

int computer_use_redact_text(const char *input, char *out, size_t out_len)
{
   if (!out || out_len == 0)
      return -1;
   out[0] = '\0';
   if (!input)
      return 0;

   int redacted = 0;
   size_t pos = 0;
   for (const char *p = input; *p && pos + 1 < out_len;)
   {
      if (strncmp(p, "sk-", 3) == 0 || strncmp(p, "ghp_", 4) == 0)
      {
         append_redacted(out, out_len, &pos, "[REDACTED]");
         while (*p && !isspace((unsigned char)*p))
            p++;
         redacted = 1;
         continue;
      }
      if (contains_ci(p, "password=") || contains_ci(p, "token=") || contains_ci(p, "api_key="))
      {
         const char *eq = strchr(p, '=');
         if (eq && eq - p < 32)
         {
            snprintf(out + pos, out_len - pos, "%.*s=[REDACTED]", (int)(eq - p), p);
            pos = strlen(out);
            p = eq + 1;
            while (*p && !isspace((unsigned char)*p) && *p != '&')
               p++;
            redacted = 1;
            continue;
         }
      }
      out[pos++] = *p++;
      out[pos] = '\0';
   }
   return redacted;
}
